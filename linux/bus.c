// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - the engine's bus on URBs
 *
 * The engine's transfer buffers are each their own kmalloc (ozzy_engine_alloc_memory: DMA-capable,
 * OZZY_DMA_ALIGN apart); the shared memory stays CPU only. URBs are allocated once per
 * pipe and reused; completions hand the result to the engine, which resubmits and moves PCM
 * and MIDI right there, then the PCM and MIDI sides catch up (ozzy_pcm_progress, ozzy_midi_service).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/version.h>

#include "ozzy.h"

#define OZZY_CTRL_BUF  64

static struct ozzy_urb_bus *to_lbus(void *ctx)
{
	return ctx;
}

static struct ozzy_urb_pipe *ozzy_urb_find(struct ozzy_urb_bus *bus, uint8_t address)
{
	unsigned int i;

	for (i = 0; i < bus->num_pipes; i++)
		if (bus->pipes[i].address == address)
			return &bus->pipes[i];
	return NULL;
}

/* A killed, unlinked or disconnected URB is "aborted" to the engine: no resubmit */
static int ozzy_urb_status(int status)
{
	switch (status) {
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return OZZY_STATUS_ABORTED;
	default:
		return status;
	}
}

/* Interrupt context: release the URB, hand the result to the engine, then let PCM and MIDI catch up */
static void ozzy_urb_complete(struct urb *urb)
{
	struct ozzy_urb *u = urb->context;
	struct ozzy_chip *chip = u->chip;
	struct ozzy_urb_bus *bus = &chip->bus;
	struct ozzy_urb_pipe *p = &bus->pipes[u->pipe];
	void *xfer = u->xfer;
	int status = ozzy_urb_status(urb->status);
	unsigned int i;

	atomic_inc(&bus->in_completion);
	if (p->type == USB_ENDPOINT_XFER_ISOC) {
		struct ozzy_isoc_packet *packets = u->packets;
		uint64_t now = ktime_get_ns();

		for (i = 0; i < (unsigned int)urb->number_of_packets; i++) {
			packets[i].actual = urb->iso_frame_desc[i].actual_length;
			packets[i].status = urb->iso_frame_desc[i].status;
			packets[i].time = now;
		}
		clear_bit(u->index, &p->busy);
		ozzy_engine_isoc_complete(chip->engine, p->address, xfer, status, packets, urb->number_of_packets);
	} else {
		clear_bit(u->index, &p->busy);
		ozzy_engine_complete(chip->engine, p->address, xfer, status, urb->actual_length);
	}
	ozzy_midi_service(chip);
	atomic_dec(&bus->in_completion);
}

/* A free URB of @p, its bit claimed; NULL if all are in flight */
static struct ozzy_urb *ozzy_urb_claim(struct ozzy_urb_pipe *p)
{
	unsigned int i;

	for (i = 0; i < OZZY_URBS_PER_PIPE; i++)
		if (p->urbs[i].urb && !test_and_set_bit(i, &p->busy))
			return &p->urbs[i];
	return NULL;
}

static bool ozzy_urb_submit(void *ctx, uint8_t pipe, void *buf, uint32_t size, void *xfer)
{
	struct ozzy_urb_bus *bus = to_lbus(ctx);
	struct ozzy_urb_pipe *p = ozzy_urb_find(bus, pipe);
	struct ozzy_urb *u;
	int ret;

	if (!p || bus->chip->gone)
		return false;
	u = ozzy_urb_claim(p);
	if (!u)
		return false;
	u->xfer = xfer;
	if (p->type == USB_ENDPOINT_XFER_INT)
		usb_fill_int_urb(u->urb, bus->dev, p->usb_pipe, buf, size, ozzy_urb_complete, u, p->interval);
	else
		usb_fill_bulk_urb(u->urb, bus->dev, p->usb_pipe, buf, size, ozzy_urb_complete, u);
	usb_anchor_urb(u->urb, &bus->anchor);
	ret = usb_submit_urb(u->urb, GFP_ATOMIC);
	if (ret < 0) {
		usb_unanchor_urb(u->urb);
		clear_bit(u->index, &p->busy);
		if (ret != -ENODEV && ret != -EPERM)
			dev_err_ratelimited(&bus->dev->dev, "[ozzy] pipe 0x%02x submit failed (%d)\n", pipe, ret);
		return false;
	}
	return true;
}

/* Isochronous: the engine's packets one after the other in @buf, which they never wrap (the
 * engine's per-transfer buffers on Linux) */
static bool ozzy_urb_submit_isoc(void *ctx, uint8_t pipe, void *buf, uint32_t buf_size, struct ozzy_isoc_packet *packets,
			     uint32_t count, void *xfer)
{
	struct ozzy_urb_bus *bus = to_lbus(ctx);
	struct ozzy_urb_pipe *p = ozzy_urb_find(bus, pipe);
	struct ozzy_urb *u;
	struct urb *urb;
	uint32_t i, end = 0;
	int ret;

	if (!p || bus->chip->gone || !count || count > OZZY_MAX_ISOC_PACKETS)
		return false;
	for (i = 0; i < count; i++) {
		if (packets[i].offset + packets[i].length > buf_size)
			return false;
		end = max(end, packets[i].offset + packets[i].length);
	}
	u = ozzy_urb_claim(p);
	if (!u)
		return false;
	u->xfer = xfer;
	u->packets = packets;
	urb = u->urb;
	urb->dev = bus->dev;
	urb->pipe = p->usb_pipe;
	urb->transfer_flags = URB_ISO_ASAP;
	urb->transfer_buffer = buf;
	urb->transfer_buffer_length = end;
	urb->number_of_packets = count;
	urb->interval = 1 << (clamp_t(unsigned int, p->interval, 1, 16) - 1);
	urb->complete = ozzy_urb_complete;
	urb->context = u;
	for (i = 0; i < count; i++) {
		urb->iso_frame_desc[i].offset = packets[i].offset;
		urb->iso_frame_desc[i].length = packets[i].length;
	}
	usb_anchor_urb(urb, &bus->anchor);
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		usb_unanchor_urb(urb);
		clear_bit(u->index, &p->busy);
		if (ret != -ENODEV && ret != -EPERM)
			dev_err_ratelimited(&bus->dev->dev, "[ozzy] isoc pipe 0x%02x submit failed (%d)\n", pipe, ret);
		return false;
	}
	return true;
}

/* Process context (the handshake): through a kmalloc'd buffer, as the engine passes stack memory */
static bool ozzy_urb_control(void *ctx, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void *data, uint16_t len)
{
	struct ozzy_urb_bus *bus = to_lbus(ctx);
	bool in = type & USB_DIR_IN;
	int ret;

	if (len > OZZY_CTRL_BUF)
		return false;
	if (!in && len)
		memcpy(bus->ctrl_buf, data, len);
	ret = usb_control_msg(bus->dev, in ? usb_rcvctrlpipe(bus->dev, 0) : usb_sndctrlpipe(bus->dev, 0), req, type,
			      val, idx, len ? bus->ctrl_buf : NULL, len, 5000);
	if (ret < 0) {
		dev_err(&bus->dev->dev, "[ozzy] control request 0x%02x (type 0x%02x, wValue 0x%04x) failed (%d)\n",
			req, type, val, ret);
		return false;
	}
	if (in && len)
		memcpy(data, bus->ctrl_buf, len);
	return true;
}

static int ozzy_urb_clear_halt(void *ctx, uint8_t endpoint)
{
	struct ozzy_urb_bus *bus = to_lbus(ctx);
	unsigned int num = endpoint & USB_ENDPOINT_NUMBER_MASK;

	return usb_clear_halt(bus->dev, (endpoint & USB_DIR_IN) ? usb_rcvbulkpipe(bus->dev, num) :
								  usb_sndbulkpipe(bus->dev, num));
}

static enum hrtimer_restart ozzy_urb_tick(struct hrtimer *timer)
{
	struct ozzy_urb_bus *bus = container_of(timer, struct ozzy_urb_bus, timer);

	if (!READ_ONCE(bus->timer_on))
		return HRTIMER_NORESTART;
	ozzy_engine_timer(bus->chip->engine);
	ozzy_midi_service(bus->chip);
	hrtimer_forward_now(timer, bus->timer_interval);
	return HRTIMER_RESTART;
}

static bool ozzy_urb_start_timer(void *ctx, uint32_t interval_us)
{
	struct ozzy_urb_bus *bus = to_lbus(ctx);

	bus->timer_interval = ns_to_ktime((u64)interval_us * NSEC_PER_USEC);
	WRITE_ONCE(bus->timer_on, true);
	hrtimer_start(&bus->timer, bus->timer_interval, HRTIMER_MODE_REL_SOFT);
	return true;
}

/* Waits for a call in progress */
static void ozzy_urb_stop_timer(void *ctx)
{
	struct ozzy_urb_bus *bus = to_lbus(ctx);

	WRITE_ONCE(bus->timer_on, false);
	hrtimer_cancel(&bus->timer);
}

static void ozzy_urb_pcm_progress(void *ctx, enum ozzy_pcm_dir dir, uint64_t frames)
{
	ozzy_pcm_progress(to_lbus(ctx)->chip, dir, frames);
}

static void ozzy_urb_sleep(void *ctx, unsigned int ms)
{
	msleep(ms);
}

static uint64_t ozzy_urb_time(void *ctx)
{
	return ktime_get_ns();
}

/* Process context: kill everything in flight; the engine has stopped resubmitting */
static void ozzy_urb_abort(void *ctx)
{
	usb_kill_anchored_urbs(&to_lbus(ctx)->anchor);
}

static const struct ozzy_bus_ops ozzy_urb_ops = {
	.submit       = ozzy_urb_submit,
	.submit_isoc  = ozzy_urb_submit_isoc,
	.control      = ozzy_urb_control,
	.clear_halt   = ozzy_urb_clear_halt,
	.start_timer  = ozzy_urb_start_timer,
	.stop_timer   = ozzy_urb_stop_timer,
	.pcm_progress = ozzy_urb_pcm_progress,
	.sleep_ms     = ozzy_urb_sleep,
	.time         = ozzy_urb_time,
	.abort        = ozzy_urb_abort,
};

int ozzy_urb_init(struct ozzy_urb_bus *bus, struct ozzy_chip *chip)
{
	memset(bus, 0, sizeof(*bus));
	bus->bus.ops = &ozzy_urb_ops;
	bus->bus.ctx = bus;
	bus->chip = chip;
	bus->dev = chip->dev;
	init_usb_anchor(&bus->anchor);
	atomic_set(&bus->in_completion, 0);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	hrtimer_setup(&bus->timer, ozzy_urb_tick, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
#else
	hrtimer_init(&bus->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
	bus->timer.function = ozzy_urb_tick;
#endif
	bus->ctrl_buf = kmalloc(OZZY_CTRL_BUF, GFP_KERNEL);
	return bus->ctrl_buf ? 0 : -ENOMEM;
}

/* A pipe the engine found (OzzyKext::ConfigureHardware's job): its URBs, allocated once */
int ozzy_urb_add_pipe(struct ozzy_urb_bus *bus, const struct usb_endpoint_descriptor *epd)
{
	struct ozzy_urb_pipe *p;
	unsigned int num = usb_endpoint_num(epd), i;
	bool in = usb_endpoint_dir_in(epd);

	if (bus->num_pipes == OZZY_MAX_PIPES)
		return -ENOSPC;
	p = &bus->pipes[bus->num_pipes];
	p->address = epd->bEndpointAddress;
	p->type = usb_endpoint_type(epd);
	p->interval = epd->bInterval;
	switch (p->type) {
	case USB_ENDPOINT_XFER_ISOC:
		p->usb_pipe = in ? usb_rcvisocpipe(bus->dev, num) : usb_sndisocpipe(bus->dev, num);
		break;
	case USB_ENDPOINT_XFER_INT:
		p->usb_pipe = in ? usb_rcvintpipe(bus->dev, num) : usb_sndintpipe(bus->dev, num);
		break;
	default:
		p->usb_pipe = in ? usb_rcvbulkpipe(bus->dev, num) : usb_sndbulkpipe(bus->dev, num);
		break;
	}
	for (i = 0; i < OZZY_URBS_PER_PIPE; i++) {
		p->urbs[i].urb = usb_alloc_urb(p->type == USB_ENDPOINT_XFER_ISOC ? OZZY_MAX_ISOC_PACKETS : 0, GFP_KERNEL);
		if (!p->urbs[i].urb)
			return -ENOMEM;
		p->urbs[i].chip = bus->chip;
		p->urbs[i].pipe = bus->num_pipes;
		p->urbs[i].index = i;
	}
	bus->num_pipes++;
	return 0;
}

/*
 * ozzy_urb_wait_completions - Process context: wait until no completion runs. One that started
 * before a ring was detached may still use it; once none runs, every later one sees the detach.
 */
void ozzy_urb_wait_completions(struct ozzy_urb_bus *bus)
{
	unsigned int i;

	for (i = 0; i < 100 && atomic_read(&bus->in_completion); i++)
		usleep_range(500, 1000);
}

void ozzy_urb_free(struct ozzy_urb_bus *bus)
{
	unsigned int i, k;

	for (i = 0; i < bus->num_pipes; i++)
		for (k = 0; k < OZZY_URBS_PER_PIPE; k++)
			usb_free_urb(bus->pipes[i].urbs[k].urb);
	bus->num_pipes = 0;
	kfree(bus->ctrl_buf);
	bus->ctrl_buf = NULL;
}
