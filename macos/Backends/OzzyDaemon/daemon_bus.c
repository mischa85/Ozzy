/* SPDX-License-Identifier: MIT */
/*
 * The daemon's bus on IOUSBLib (daemon_bus.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "daemon_bus.h"
#include "../../../engine/shared/ozzy_log.h"
#include <IOKit/IOCFPlugIn.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <unistd.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"   /* IOUSBLib, which 10.11 has */

/* The 1 ms frame a pipe's first isochronous transfer starts on, this far ahead (as KextBus) */
#define ISOC_START_FRAMES 10

static uint64_t abs_from_ns(uint64_t ns)
{
	static mach_timebase_info_data_t tb;

	if (!tb.denom)
		mach_timebase_info(&tb);
	return ns * tb.denom / tb.numer;
}

static uint64_t abs_time(AbsoluteTime t)
{
	uint64_t v;

	memcpy(&v, &t, sizeof(v));
	return v;
}

/*
 * IOUSBLib reports a short read as kIOReturnUnderrun (a feedback packet of 3 bytes on a 4-byte
 * endpoint, a bulk read that ended early): the bytes say how much came, as on the other
 * platforms, where it isn't an error
 */
static int usb_status(IOReturn r)
{
	return r == kIOReturnUnderrun ? 0 : (int)r;
}

static struct daemon_bus_pipe *find_pipe(struct daemon_bus *b, uint8_t address)
{
	struct daemon_bus_pipe *p = &b->pipe[daemon_bus_slot(address)];

	return p->intf ? p : NULL;
}

static void note_done(struct daemon_bus *b)
{
	__atomic_fetch_sub(&b->pending, 1, __ATOMIC_RELEASE);
}

/* --- Bulk and interrupt --- */

struct transfer {
	struct daemon_bus *bus;
	uint8_t pipe;
	void *xfer;
};

static void transfer_done(void *refcon, IOReturn result, void *arg0)
{
	struct transfer *t = refcon;
	struct daemon_bus *b = t->bus;
	uint8_t pipe = t->pipe;
	void *xfer = t->xfer;
	int status = usb_status(result);

	free(t);
	if (status && status != OZZY_STATUS_ABORTED)
		ozzy_log("pipe 0x%02x error: 0x%x", pipe, status);
	if (b->engine)
		ozzy_engine_complete(b->engine, pipe, xfer, status, (uint32_t)(uintptr_t)arg0);
	note_done(b);
}

static bool bus_submit(void *ctx, uint8_t pipe, void *buf, uint32_t size, void *xfer)
{
	struct daemon_bus *b = ctx;
	struct daemon_bus_pipe *p = find_pipe(b, pipe);
	struct transfer *t;
	IOReturn r;

	if (!p)
		return false;
	t = malloc(sizeof(*t));
	if (!t)
		return false;
	t->bus = b;
	t->pipe = pipe;
	t->xfer = xfer;
	__atomic_fetch_add(&b->pending, 1, __ATOMIC_ACQUIRE);
	r = (pipe & 0x80) ? (*p->intf)->ReadPipeAsync(p->intf, p->ref, buf, size, transfer_done, t)
			  : (*p->intf)->WritePipeAsync(p->intf, p->ref, buf, size, transfer_done, t);
	if (r != kIOReturnSuccess) {
		__atomic_fetch_sub(&b->pending, 1, __ATOMIC_RELEASE);
		free(t);
		ozzy_log("pipe 0x%02x submit failed: 0x%x", pipe, r);
		return false;
	}
	return true;
}

/* --- Isochronous --- */

/*
 * An isochronous transfer: IOUSBLib takes the packets back to back from one buffer (a frame
 * list entry per service interval, numbered by 1 ms frames), so packets that run round the end
 * of the ring go through @bounce. Its frame entries carry no time: each packet's is the end of
 * its interval on the bus clock, from the frame number IOUSBLib pairs with a time at submit.
 */
struct isoc_transfer {
	struct daemon_bus *bus;
	uint8_t pipe;
	void *xfer;
	struct ozzy_isoc_packet *packets;
	uint32_t count;
	uint8_t *buf;                   /* the engine's buffer */
	uint32_t buf_size;
	uint8_t *bounce;                /* NULL: the packets are in @buf as they are */
	uint32_t bytes;
	uint64_t first_uframe;          /* microframe the transfer starts on */
	uint32_t interval_uframes;      /* microframes per packet */
	uint64_t ref_uframe;            /* the bus clock: microframe @ref_time started */
	uint64_t ref_time;
	IOUSBIsocFrame frames[OZZY_MAX_ISOC_PACKETS];
};

/* Copy the packets that wrap between the ring (@to_ring) and the transfer's own buffer */
static void bounce_copy(struct isoc_transfer *t, bool to_ring)
{
	uint32_t start = t->packets[0].offset;
	uint32_t head = t->buf_size - start;   /* bytes up to the end of the ring */

	if (to_ring) {
		memcpy(t->buf + start, t->bounce, head);
		memcpy(t->buf, t->bounce + head, t->bytes - head);
	} else {
		memcpy(t->bounce, t->buf + start, head);
		memcpy(t->bounce + head, t->buf, t->bytes - head);
	}
}

static void isoc_done(void *refcon, IOReturn result, void *arg0)
{
	struct isoc_transfer *t = refcon;
	struct daemon_bus *b = t->bus;
	uint8_t pipe = t->pipe;
	void *xfer = t->xfer;
	struct ozzy_isoc_packet *packets = t->packets;
	uint32_t count = t->count, i;
	int status = usb_status(result);
	uint64_t uframe_ticks = abs_from_ns(125000);

	(void)arg0;
	for (i = 0; i < count; i++) {
		uint64_t end = t->first_uframe + (uint64_t)(i + 1) * t->interval_uframes;

		packets[i].actual = t->frames[i].frActCount;
		packets[i].status = usb_status(t->frames[i].frStatus);
		packets[i].time = t->ref_time && end >= t->ref_uframe ? t->ref_time + (end - t->ref_uframe) * uframe_ticks
								     : 0;
	}
	if (t->bounce && (pipe & 0x80) && status != OZZY_STATUS_ABORTED)
		bounce_copy(t, true);
	free(t->bounce);
	free(t);

	if (status && status != OZZY_STATUS_ABORTED)
		ozzy_log("isoc pipe 0x%02x error: 0x%x", pipe, status);
	if (b->engine)
		ozzy_engine_isoc_complete(b->engine, pipe, xfer, status, packets, count);
	note_done(b);
}

/* Where a transfer of @count packets starts: where the pipe's previous one ended, the first
 * ISOC_START_FRAMES ahead of the bus (as KextBus::IsocFrameNumber) */
static uint64_t isoc_frame(struct daemon_bus *b, struct daemon_bus_pipe *p, uint8_t pipe, uint32_t count,
			   uint32_t interval_uframes, uint64_t now)
{
	uint64_t *next = &b->isoc_next_frame[daemon_bus_slot(pipe)];
	uint64_t uframes = (uint64_t)count * interval_uframes;
	uint64_t frames = (uframes + 7) / 8;
	uint64_t first;

	if (*next <= now)
		*next = now + ISOC_START_FRAMES;   /* the first transfer, or the pipe ran dry */
	first = *next;
	*next += frames;
	return first;
}

static bool bus_submit_isoc(void *ctx, uint8_t pipe, void *buf, uint32_t buf_size, struct ozzy_isoc_packet *packets,
			    uint32_t count, void *xfer)
{
	struct daemon_bus *b = ctx;
	struct daemon_bus_pipe *p = find_pipe(b, pipe);
	struct isoc_transfer *t;
	uint32_t start, i, interval;
	uint64_t now = 0, frame;
	AbsoluteTime at;
	uint8_t *data;
	IOReturn r;

	if (!p || !count || count > OZZY_MAX_ISOC_PACKETS || !buf_size)
		return false;
	t = calloc(1, sizeof(*t));
	if (!t)
		return false;
	t->bus = b;
	t->pipe = pipe;
	t->xfer = xfer;
	t->packets = packets;
	t->count = count;
	t->buf = buf;
	t->buf_size = buf_size;

	/* The packets back to back from the first one's offset, maybe round the end of @buf */
	start = packets[0].offset;
	for (i = 0; i < count; i++) {
		if (packets[i].offset != (start + t->bytes) % buf_size) {
			ozzy_log("isoc pipe 0x%02x: packet %u not contiguous", pipe, i);
			free(t);
			return false;
		}
		t->frames[i].frReqCount = (UInt16)packets[i].length;
		t->bytes += packets[i].length;
	}
	if (start >= buf_size || t->bytes > buf_size) {
		free(t);
		return false;
	}
	if (start + t->bytes > buf_size) {
		t->bounce = malloc(t->bytes);
		if (!t->bounce) {
			free(t);
			return false;
		}
		if (!(pipe & 0x80))
			bounce_copy(t, false);
		data = t->bounce;
	} else {
		data = (uint8_t *)buf + start;
	}

	/* A packet per service interval: 2^(bInterval-1) microframes at High Speed, frames below */
	interval = p->interval ? 1u << (p->interval - 1) : 1;
	t->interval_uframes = b->high_speed ? interval : interval * 8;
	if ((*p->intf)->GetBusFrameNumberWithTime(p->intf, &now, &at) == kIOReturnSuccess) {
		t->ref_uframe = now * 8;
		t->ref_time = abs_time(at);
	} else if ((*p->intf)->GetBusFrameNumber(p->intf, &now, &at) != kIOReturnSuccess) {
		free(t->bounce);
		free(t);
		return false;
	}
	frame = isoc_frame(b, p, pipe, count, t->interval_uframes, now);
	t->first_uframe = frame * 8;

	__atomic_fetch_add(&b->pending, 1, __ATOMIC_ACQUIRE);
	r = (pipe & 0x80) ? (*p->intf)->ReadIsochPipeAsync(p->intf, p->ref, data, frame, count, t->frames, isoc_done, t)
			  : (*p->intf)->WriteIsochPipeAsync(p->intf, p->ref, data, frame, count, t->frames, isoc_done, t);
	if (r != kIOReturnSuccess) {
		__atomic_fetch_sub(&b->pending, 1, __ATOMIC_RELEASE);
		ozzy_log("isoc pipe 0x%02x submit failed: 0x%x", pipe, r);
		b->isoc_next_frame[daemon_bus_slot(pipe)] = frame;
		free(t->bounce);
		free(t);
		return false;
	}
	return true;
}

/* --- Control, halts, abort --- */

static bool bus_control(void *ctx, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void *data,
			uint16_t length)
{
	struct daemon_bus *b = ctx;
	IOUSBDevRequestTO req = { 0 };
	IOReturn r;

	req.bmRequestType = type;
	req.bRequest = request;
	req.wValue = value;
	req.wIndex = index;
	req.wLength = length;
	req.pData = data;
	req.noDataTimeout = 5000;
	req.completionTimeout = 5000;
	r = (*b->dev)->DeviceRequestTO(b->dev, &req);
	return r == kIOReturnSuccess;
}

/* CLEAR_FEATURE(ENDPOINT_HALT) to the device and a reset of the host side of the pipe */
static int bus_clear_halt(void *ctx, uint8_t endpoint)
{
	struct daemon_bus_pipe *p = find_pipe(ctx, endpoint);

	if (!p)
		return -ENODEV;
	return (*p->intf)->ClearPipeStallBothEnds(p->intf, p->ref) == kIOReturnSuccess ? 0 : -EIO;
}

/*
 * Abort every pipe and wait until the USB thread has delivered the completions (aborted). Never
 * from the USB thread itself: it would wait for itself.
 */
static void bus_abort(void *ctx)
{
	struct daemon_bus *b = ctx;
	int i;

	for (i = 0; i < DAEMON_BUS_PIPE_SLOTS; i++)
		if (b->pipe[i].intf)
			(*b->pipe[i].intf)->AbortPipe(b->pipe[i].intf, b->pipe[i].ref);
	if (pthread_equal(pthread_self(), b->thread)) {
		ozzy_log("abort from the USB thread: not waiting");
		return;
	}
	for (i = 0; i < 2000 && __atomic_load_n(&b->pending, __ATOMIC_ACQUIRE) > 0; i++)
		usleep(1000);
	if (__atomic_load_n(&b->pending, __ATOMIC_ACQUIRE) > 0)
		ozzy_log("%d transfers still pending after abort", b->pending);
}

/* --- Timer, clock --- */

static void timer_fired(CFRunLoopTimerRef timer, void *info)
{
	struct daemon_bus *b = info;

	(void)timer;
	b->timer_busy = true;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (b->timer_on && b->engine)
		ozzy_engine_timer(b->engine);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	b->timer_busy = false;
}

/* A repeating run loop timer on the USB thread: CF keeps its schedule (fire dates advance by the
 * interval; missed ones are skipped), as KextBus's absolute deadlines */
static bool bus_start_timer(void *ctx, uint32_t interval_us)
{
	struct daemon_bus *b = ctx;
	CFRunLoopTimerContext context = { 0, b, NULL, NULL, NULL };
	CFTimeInterval interval = interval_us / 1e6;

	if (b->timer)
		return true;
	b->timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + interval, interval, 0, 0,
					timer_fired, &context);
	if (!b->timer)
		return false;
	CFRunLoopTimerSetTolerance(b->timer, 0);
	b->timer_on = true;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	CFRunLoopAddTimer(b->run_loop, b->timer, kCFRunLoopDefaultMode);
	CFRunLoopWakeUp(b->run_loop);
	return true;
}

/* Invalidate, then wait for a call in progress: it sets busy before it looks at on, and stop
 * clears on before it looks at busy, so one of them sees the other */
static void bus_stop_timer(void *ctx)
{
	struct daemon_bus *b = ctx;

	if (!b->timer)
		return;
	b->timer_on = false;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	CFRunLoopTimerInvalidate(b->timer);
	if (!pthread_equal(pthread_self(), b->thread))
		while (b->timer_busy)
			usleep(100);
	CFRelease(b->timer);
	b->timer = NULL;
}

static void bus_sleep(void *ctx, unsigned int ms)
{
	(void)ctx;
	usleep(ms * 1000);
}

static uint64_t bus_time(void *ctx)
{
	(void)ctx;
	return mach_absolute_time();
}

static uint64_t bus_time_hz(void *ctx)
{
	(void)ctx;
	return abs_from_ns(1000000000ull);
}

/* --- Interfaces and pipes --- */

static bool select_alternate(daemon_usb_interface_t intf, uint8_t alt)
{
	IOReturn r = kIOReturnError;
	int attempt;

	/* A stall asked again, as the handshake's requests (up to 10 times, 20 ms apart) */
	for (attempt = 0; attempt < 10; attempt++) {
		if (attempt)
			usleep(20000);
		r = (*intf)->SetAlternateInterface(intf, alt);
		if (r != kIOUSBPipeStalled && r != kIOReturnNotResponding)
			break;
	}
	if (r != kIOReturnSuccess) {
		uint8_t number = 0;

		(*intf)->GetInterfaceNumber(intf, &number);
		ozzy_log("interface %u: alternate setting %u failed (0x%x)", number, alt, r);
	}
	return r == kIOReturnSuccess;
}

/* Every pipe of the claimed interfaces at their current settings, by endpoint address */
static void map_pipes(struct daemon_bus *b)
{
	int i, slot;

	memset(b->pipe, 0, sizeof(b->pipe));
	memset(b->isoc_next_frame, 0, sizeof(b->isoc_next_frame));
	for (i = 0; i < b->intf_count; i++) {
		daemon_usb_interface_t intf = b->intf[i];
		uint8_t n = 0, ref;

		if (!intf || (*intf)->GetNumEndpoints(intf, &n) != kIOReturnSuccess)
			continue;
		for (ref = 1; ref <= n; ref++) {
			uint8_t dir, number, type, interval;
			uint16_t mps;

			if ((*intf)->GetPipeProperties(intf, ref, &dir, &number, &type, &mps, &interval) != kIOReturnSuccess)
				continue;
			slot = daemon_bus_slot((uint8_t)(number | (dir == kUSBIn ? 0x80 : 0)));
			b->pipe[slot].intf = intf;
			b->pipe[slot].ref = ref;
			b->pipe[slot].interval = interval;
		}
	}
}

/* SET_INTERFACE 0, then @alts[i] again, on each interface i whose setting isn't 0
 * (ozzy_engine_reselect: suspended, nothing in flight) */
static bool bus_reselect(void *ctx, const uint8_t alts[4])
{
	struct daemon_bus *b = ctx;
	bool ok = true;
	int i;

	for (i = 0; i < b->intf_count; i++) {
		if (!b->intf[i] || !alts[i])
			continue;
		ok &= select_alternate(b->intf[i], 0);
		ok &= select_alternate(b->intf[i], alts[i]);
		usleep(50000);   /* as after the first selection */
	}
	map_pipes(b);
	return ok;
}

/* No PCM rings (the HAL writes the shared memory itself) and no stats_changed (the HAL polls the
 * stats), as KextBus */
static const struct ozzy_bus_ops daemon_bus_ops = {
	.submit = bus_submit,
	.submit_isoc = bus_submit_isoc,
	.control = bus_control,
	.clear_halt = bus_clear_halt,
	.start_timer = bus_start_timer,
	.stop_timer = bus_stop_timer,
	.sleep_ms = bus_sleep,
	.time = bus_time,
	.abort = bus_abort,
	.time_hz = bus_time_hz,
	.reselect = bus_reselect,
};

void daemon_bus_init(struct daemon_bus *b, daemon_usb_device_t dev, CFRunLoopRef run_loop, pthread_t thread,
		     struct ozzy_engine *engine)
{
	UInt8 speed = 0;

	b->bus.ops = &daemon_bus_ops;
	b->bus.ctx = b;
	b->dev = dev;
	b->run_loop = run_loop;
	b->thread = thread;
	b->engine = engine;
	if ((*dev)->GetDeviceSpeed(dev, &speed) == kIOReturnSuccess)
		b->high_speed = speed >= kUSBDeviceSpeedHigh;
}

enum ozzy_usb_speed daemon_bus_speed(struct daemon_bus *b)
{
	UInt8 speed = 0;

	if ((*b->dev)->GetDeviceSpeed(b->dev, &speed) != kIOReturnSuccess)
		return OZZY_USB_SPEED_UNKNOWN;
	switch (speed) {
	case kUSBDeviceSpeedLow:
		return OZZY_USB_SPEED_LOW;
	case kUSBDeviceSpeedFull:
		return OZZY_USB_SPEED_FULL;
	case kUSBDeviceSpeedHigh:
		return OZZY_USB_SPEED_HIGH;
	default:
		return OZZY_USB_SPEED_SUPER;
	}
}

/* An interface of the device by bInterfaceNumber, open (seized from any driver that has it) */
static daemon_usb_interface_t open_interface(struct daemon_bus *b, uint8_t number)
{
	IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
					  kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
	daemon_usb_interface_t found = NULL;
	io_iterator_t it = IO_OBJECT_NULL;
	io_service_t service;

	if ((*b->dev)->CreateInterfaceIterator(b->dev, &req, &it) != kIOReturnSuccess)
		return NULL;
	while (!found && (service = IOIteratorNext(it))) {
		IOCFPlugInInterface **plug = NULL;
		daemon_usb_interface_t intf = NULL;
		SInt32 score;
		uint8_t n = 0xFF;

		if (IOCreatePlugInInterfaceForService(service, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID,
						      &plug, &score) == KERN_SUCCESS && plug) {
			(*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650), (LPVOID *)&intf);
			IODestroyPlugInInterface(plug);
		}
		IOObjectRelease(service);
		if (!intf)
			continue;
		if ((*intf)->GetInterfaceNumber(intf, &n) == kIOReturnSuccess && n == number) {
			IOReturn r = (*intf)->USBInterfaceOpenSeize(intf);

			if (r == kIOReturnSuccess)
				found = intf;
			else
				ozzy_log("interface %u: can't open it (0x%x)", number, r);
		}
		if (found != intf)
			(*intf)->Release(intf);
	}
	IOObjectRelease(it);
	return found;
}

/*
 * The endpoints of interface @number at alternate setting @alt, from the configuration
 * descriptor, as the engine wants them (wMaxPacketSize as is, with its high-bandwidth bits)
 */
static unsigned int list_endpoints(struct daemon_bus *b, uint8_t number, uint8_t alt, daemon_usb_interface_t intf,
				   struct ozzy_usb_endpoint *eps, unsigned int max)
{
	IOUSBConfigurationDescriptorPtr cfg = NULL;
	const uint8_t *p, *end;
	bool in_it = false;
	unsigned int n = 0;

	if ((*b->dev)->GetConfigurationDescriptorPtr(b->dev, 0, &cfg) != kIOReturnSuccess || !cfg)
		return 0;
	p = (const uint8_t *)cfg;
	end = p + USBToHostWord(cfg->wTotalLength);
	for (p += cfg->bLength; p + 2 <= end && p[0] && n < max; p += p[0]) {
		if (p[1] == kUSBInterfaceDesc && p + 4 <= end)
			in_it = p[2] == number && p[3] == alt;
		else if (in_it && p[1] == kUSBEndpointDesc && p + 7 <= end) {
			eps[n].intf = number;
			eps[n].address = p[2];
			eps[n].attributes = p[3];
			eps[n].max_packet = (uint16_t)(p[4] | (p[5] << 8));
			eps[n].interval = p[6];
			eps[n++].handle = intf;
		}
	}
	return n;
}

bool daemon_bus_configure(struct daemon_bus *b)
{
	const struct ozzy_device_profile *profile = &b->engine->profile;
	const struct ozzy_usb_endpoint *bound[OZZY_PIPE_ROLES];
	struct ozzy_usb_endpoint eps[32];
	unsigned int n = 0;
	IOReturn r;
	int i, tries;

	/* Configuration 1, as the kext: the interfaces come up again (published asynchronously) */
	r = (*b->dev)->SetConfiguration(b->dev, 1);
	if (r != kIOReturnSuccess)
		ozzy_log("SET_CONFIGURATION 1 failed (0x%x)", r);
	usleep(500000);

	for (i = 0; i < profile->interface_count && i < DAEMON_BUS_INTERFACES; i++) {
		for (tries = 0; tries < 10 && !b->intf[i]; tries++) {
			if (tries)
				usleep(100000);
			b->intf[i] = open_interface(b, profile->interfaces[i]);
		}
		if (!b->intf[i]) {
			ozzy_log("interface %u not found", profile->interfaces[i]);
			return false;
		}
		b->intf_count = i + 1;
		if (profile->alt_settings[i]) {
			if (!select_alternate(b->intf[i], profile->alt_settings[i]))
				return false;
			usleep(50000);
		}
		r = (*b->intf[i])->CreateInterfaceAsyncEventSource(b->intf[i], &b->source[i]);
		if (r != kIOReturnSuccess || !b->source[i]) {
			ozzy_log("interface %u: no async event source (0x%x)", profile->interfaces[i], r);
			return false;
		}
		CFRunLoopAddSource(b->run_loop, b->source[i], kCFRunLoopDefaultMode);
	}

	/* Every endpoint of the claimed interfaces to the engine, which picks its pipes */
	for (i = 0; i < b->intf_count; i++)
		n += list_endpoints(b, profile->interfaces[i], profile->alt_settings[i], b->intf[i], eps + n, 32 - n);
	ozzy_engine_bind_endpoints(b->engine, eps, n, bound);
	map_pipes(b);
	for (i = 0; i < OZZY_PIPE_ROLES; i++) {
		if (bound[i] && !find_pipe(b, bound[i]->address)) {
			ozzy_log("no pipe for endpoint 0x%02x", bound[i]->address);
			return false;
		}
	}
	CFRunLoopWakeUp(b->run_loop);
	return true;
}

void daemon_bus_close(struct daemon_bus *b)
{
	int i;

	bus_stop_timer(b);
	memset(b->pipe, 0, sizeof(b->pipe));
	for (i = 0; i < DAEMON_BUS_INTERFACES; i++) {
		if (b->source[i]) {
			CFRunLoopRemoveSource(b->run_loop, b->source[i], kCFRunLoopDefaultMode);
			CFRelease(b->source[i]);
			b->source[i] = NULL;
		}
		if (b->intf[i]) {
			(*b->intf[i])->USBInterfaceClose(b->intf[i]);
			(*b->intf[i])->Release(b->intf[i]);
			b->intf[i] = NULL;
		}
	}
	b->intf_count = 0;
}
