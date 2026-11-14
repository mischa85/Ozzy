/* SPDX-License-Identifier: MIT */
/*
 * The devices the daemon drives (daemon_device.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "daemon_device.h"
#include "daemon_server.h"
#include "daemon_bus.h"
#include "../../Shared/ozzy_daemon.h"
#include "../../../engine/shared/ozzy_log.h"
#include "../../../common/ozzy_devices.h"
#include "../../../common/ozzy_version.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"   /* IOUSBLib, which 10.11 has */

typedef daemon_usb_device_t usb_device_t;

struct daemon_device {
	io_service_t service;
	uint64_t registry_id;
	uint32_t location_id;
	const struct ozzy_device *model;
	usb_device_t usb;
	bool usb_open;
	int slot;                            /* in devices[] and the table */

	char shm_name[32];
	struct ozzy_shared_memory *shm;
	size_t shm_size;

	/* The calls that wait (settings, rate): one at a time, as the kext's settings lock */
	dispatch_queue_t queue;
	volatile bool closing;

	/* The USB completions' thread: its run loop gets the interfaces' async event sources */
	pthread_t usb_thread;
	bool usb_thread_started;
	CFRunLoopRef usb_run_loop;
	dispatch_semaphore_t usb_ready;
	volatile bool usb_stop;

	struct ozzy_engine *engine;
	bool attached;                       /* ozzy_engine_attach done: it can be stopped */
	struct daemon_bus bus;
	volatile bool running;               /* engine started and device present (the kext's mRunning) */
	bool sleeping;                       /* suspended for system sleep (on @queue) */

	/* Stall watchdog, on @queue (so never in the middle of a call): as the kext's */
	dispatch_source_t watch;
	uint64_t recovered_at;               /* mach time of the last recovery (0: none) */
};

static pthread_mutex_t devices_lock = PTHREAD_MUTEX_INITIALIZER;
static struct daemon_device *devices[OZZY_DAEMON_MAX_DEVICES];
static struct ozzy_daemon_table *table;
static size_t table_size;

/* --- POSIX shared memory --- */

/*
 * A new object @name of @size bytes (rounded up to pages), zeroed and mapped; one left by a
 * daemon that crashed is replaced. @mode as is (no umask): the clients run as _coreaudiod
 * (OzzyHAL) and as the logged-in user (OzzyMIDI, in MIDIServer).
 */
static void *shm_create(const char *name, size_t size, mode_t mode, size_t *mapped)
{
	size_t page = (size_t)getpagesize();
	mode_t old_mask;
	void *p;
	int fd;

	size = (size + page - 1) & ~(page - 1);
	shm_unlink(name);
	old_mask = umask(0);
	fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, mode);
	umask(old_mask);
	if (fd < 0) {
		ozzy_log("%s: shm_open failed (%d)", name, errno);
		return NULL;
	}
	if (ftruncate(fd, (off_t)size) != 0) {
		ozzy_log("%s: ftruncate failed (%d)", name, errno);
		close(fd);
		shm_unlink(name);
		return NULL;
	}
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED) {
		ozzy_log("%s: mmap failed (%d)", name, errno);
		shm_unlink(name);
		return NULL;
	}
	*mapped = size;
	return p;
}

static void shm_destroy(const char *name, void *p, size_t size)
{
	if (p)
		munmap(p, size);
	shm_unlink(name);
}

/* --- The device table (written from the main thread only) --- */

/* Entry @slot as @e (NULL: free), under the table's seqlock */
static void table_set(int slot, const struct ozzy_daemon_entry *e)
{
	uint32_t seq = table->sequence;

	ozzy_store_relaxed_u32(&table->sequence, seq + 1);
	ozzy_fence_release();
	if (e)
		table->entries[slot] = *e;
	else
		memset(&table->entries[slot], 0, sizeof(table->entries[slot]));
	ozzy_store_release_u32(&table->sequence, seq + 2);
}

bool daemon_devices_init(void)
{
	table = shm_create(OZZY_DAEMON_TABLE_NAME, sizeof(*table), 0644, &table_size);
	if (!table)
		return false;
	table->version = OZZY_DAEMON_TABLE_VERSION;
	table->daemon_pid = (uint32_t)getpid();
	ozzy_store_release_u32(&table->magic, OZZY_DAEMON_TABLE_MAGIC);
	return true;
}

/* --- Registry --- */

static void registry_string(io_service_t service, CFStringRef key, char *dst, size_t size)
{
	CFTypeRef ref = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);

	dst[0] = 0;
	if (!ref)
		return;
	if (CFGetTypeID(ref) == CFStringGetTypeID())
		CFStringGetCString((CFStringRef)ref, dst, (CFIndex)size, kCFStringEncodingUTF8);
	CFRelease(ref);
}

static uint32_t registry_u32(io_service_t service, CFStringRef key)
{
	CFTypeRef ref = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
	uint32_t value = 0;

	if (!ref)
		return 0;
	if (CFGetTypeID(ref) == CFNumberGetTypeID())
		CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt32Type, &value);
	CFRelease(ref);
	return value;
}

/* --- USB --- */

static bool usb_create(struct daemon_device *d)
{
	IOCFPlugInInterface **plug = NULL;
	SInt32 score = 0;
	kern_return_t kr;
	HRESULT hr;

	kr = IOCreatePlugInInterfaceForService(d->service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
					       &plug, &score);
	if (kr != KERN_SUCCESS || !plug) {
		ozzy_log("no USB user client (0x%x)", kr);
		return false;
	}
	hr = (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&d->usb);
	IODestroyPlugInInterface(plug);
	if (hr != S_OK || !d->usb) {
		ozzy_log("no IOUSBDeviceInterface650 (0x%x)", (unsigned)hr);
		d->usb = NULL;
		return false;
	}
	return true;
}

/*
 * Whether interface 0 is USB Audio class: a class-compliant revision under a listed ID (the
 * Dynacord CMS 600-3's HW:03), which Apple's class driver should keep, as the kext leaves it.
 * From the descriptors, before the daemon opens anything.
 */
static bool usb_class_compliant(usb_device_t usb)
{
	IOUSBConfigurationDescriptorPtr cfg = NULL;
	const uint8_t *p, *end;

	if ((*usb)->GetConfigurationDescriptorPtr(usb, 0, &cfg) != kIOReturnSuccess || !cfg)
		return false;
	p = (const uint8_t *)cfg;
	end = p + USBToHostWord(cfg->wTotalLength);
	for (p += cfg->bLength; p + 6 <= end && p[0]; p += p[0])
		if (p[1] == kUSBInterfaceDesc)
			return p[5] == kUSBAudioInterfaceClass;
	return false;
}

/*
 * Open the device, taking it from whoever has it open (Apple's composite driver lets go; a
 * kext that doesn't answer the request to close, such as OzzyKext, keeps it). Five tries, as
 * the January daemon did: right after attach the device can still be busy.
 */
static bool usb_seize(struct daemon_device *d)
{
	IOReturn ret = kIOReturnError;
	int i;

	for (i = 0; i < 5; i++) {
		if (i)
			usleep(100000);
		ret = (*d->usb)->USBDeviceOpenSeize(d->usb);
		if (ret == kIOReturnSuccess) {
			d->usb_open = true;
			return true;
		}
	}
	if (ret == kIOReturnExclusiveAccess)
		ozzy_log("another driver keeps the device open (0x%x): is OzzyKext loaded?", ret);
	else
		ozzy_log("can't open the device (0x%x)", ret);
	return false;
}

/* --- The USB thread --- */

/*
 * Real-time scheduling for the completions: the same constraints as the January daemon, which
 * ran the Xone:4D's transfers this way (a 3.33 ms period, 0.4 ms of work in it).
 */
static void usb_thread_realtime(void)
{
	mach_timebase_info_data_t tb;
	thread_time_constraint_policy_data_t policy;
	kern_return_t kr;
	mach_port_t thread = mach_thread_self();

	mach_timebase_info(&tb);
	policy.period = (uint32_t)(3333333ull * tb.denom / tb.numer);
	policy.computation = (uint32_t)(400000ull * tb.denom / tb.numer);
	policy.constraint = policy.period;
	policy.preemptible = 1;
	kr = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy,
			       THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	mach_port_deallocate(mach_task_self(), thread);
	if (kr != KERN_SUCCESS)
		ozzy_log("no real-time scheduling for the USB thread (0x%x)", kr);
}

static void *usb_thread_main(void *arg)
{
	struct daemon_device *d = arg;
	CFRunLoopSourceContext context = { 0 };
	CFRunLoopSourceRef keep;

	pthread_setname_np("ozzy usb");
	usb_thread_realtime();

	/* A run loop without sources returns at once: this one keeps it waiting */
	keep = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
	CFRunLoopAddSource(CFRunLoopGetCurrent(), keep, kCFRunLoopDefaultMode);
	d->usb_run_loop = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());
	dispatch_semaphore_signal(d->usb_ready);

	while (!d->usb_stop)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);

	CFRunLoopRemoveSource(CFRunLoopGetCurrent(), keep, kCFRunLoopDefaultMode);
	CFRelease(keep);
	return NULL;
}

static bool usb_thread_start(struct daemon_device *d)
{
	d->usb_ready = dispatch_semaphore_create(0);
	if (!d->usb_ready || pthread_create(&d->usb_thread, NULL, usb_thread_main, d) != 0) {
		ozzy_log("can't start the USB thread");
		return false;
	}
	d->usb_thread_started = true;
	dispatch_semaphore_wait(d->usb_ready, DISPATCH_TIME_FOREVER);
	return true;
}

static void usb_thread_stop(struct daemon_device *d)
{
	if (!d->usb_thread_started)
		return;
	d->usb_stop = true;
	CFRunLoopStop(d->usb_run_loop);
	CFRunLoopWakeUp(d->usb_run_loop);
	pthread_join(d->usb_thread, NULL);
	CFRelease(d->usb_run_loop);
	d->usb_run_loop = NULL;
	d->usb_thread_started = false;
}

/* --- A device --- */

static void noop(void *ctx)
{
	(void)ctx;
}

/* Undo what device_open did, as far as it got. The device is out of devices[] and the table. */
static void device_free(struct daemon_device *d)
{
	if (d->watch)
		dispatch_source_cancel(d->watch);
	if (d->queue) {
		/* Calls in progress or queued, and the watchdog, finish first; they see @closing */
		d->closing = true;
		dispatch_sync_f(d->queue, NULL, noop);
	}
	d->running = false;
	if (d->shm) {
		/* Tell the clients before the object goes: they unmap on their next check */
		ozzy_store_release_bool(&d->shm->audio.hardware_present, false);
		ozzy_store_release_bool(&d->shm->audio.driver_ready, false);
		ozzy_store_release_u32(&d->shm->magic, 0);
	}
	/* Every transfer aborted (the USB thread delivers the completions), then the interfaces */
	if (d->attached)
		ozzy_engine_stop(d->engine);
	daemon_bus_close(&d->bus);
	usb_thread_stop(d);
	free(d->engine);
	if (d->watch)
		dispatch_release(d->watch);
	if (d->usb) {
		if (d->usb_open)
			(*d->usb)->USBDeviceClose(d->usb);
		(*d->usb)->Release(d->usb);
	}
	if (d->shm)
		shm_destroy(d->shm_name, d->shm, d->shm_size);
	if (d->queue)
		dispatch_release(d->queue);
	if (d->usb_ready)
		dispatch_release(d->usb_ready);
	IOObjectRelease(d->service);
	free(d);
}

/* --- The engine --- */

/*
 * The engine for the device's family, on the daemon's bus, as OzzyKext::start: the interfaces,
 * attach, probe, the connection and the build for ozzy_info, the HAL's clock anchors, start
 */
static bool engine_start(struct daemon_device *d)
{
	size_t size = ozzy_engine_size(d->model);

	d->engine = ozzy_engine_init(size ? calloc(1, size) : NULL, d->model);
	if (!d->engine) {
		ozzy_log("no engine for %s", d->model->name);
		return false;
	}
	daemon_bus_init(&d->bus, d->usb, d->usb_run_loop, d->usb_thread, d->engine);
	ozzy_log("configuring hardware...");
	if (!daemon_bus_configure(&d->bus)) {
		ozzy_log("configuration failed");
		return false;
	}
	ozzy_engine_attach(d->engine, &d->bus.bus, d->shm);
	d->attached = true;
	/* Its channels can depend on the device (the US-366's front panel) */
	if (!ozzy_engine_probe(d->engine)) {
		ozzy_log("probe failed");
		return false;
	}
	ozzy_engine_set_usb_speed(d->engine, daemon_bus_speed(&d->bus));
	ozzy_engine_set_driver_version(d->engine, OZZY_GIT_VERSION);
	ozzy_engine_set_anchors(d->engine, true);   /* OzzyHAL's zero timestamps */
	if (!ozzy_engine_start(d->engine)) {
		ozzy_log("engine start failed");
		return false;
	}
	daemon_bus_watch_reset(&d->bus);
	d->running = true;
	ozzy_log("driver ready: %u Hz, %u in / %u out", d->shm->audio.sample_rate, d->shm->audio.input_channels,
		 d->shm->audio.output_channels);
	return true;
}

/* Take the rate or settings that changed (ozzy_engine_restart), on the queue. A device that
 * doesn't take it is dropped, as by the kext: the HAL lets it go. */
static int engine_restart(struct daemon_device *d)
{
	if (ozzy_engine_restart(d->engine)) {
		daemon_bus_watch_reset(&d->bus);
		return kIOReturnSuccess;
	}
	d->running = false;
	return kIOReturnIOError;
}

/* --- Stall watchdog (OzzyKext::WatchLocked) --- */

#define WATCH_PERIOD_MS   250
#define WATCH_DEAD_MS     500     /* ~100 transfer periods: no late completion */
#define WATCH_RECOVER_S   10      /* between recoveries: not forever on a dead device */

static uint64_t ms_to_abs(uint64_t ms)
{
	static mach_timebase_info_data_t tb;

	if (!tb.denom)
		mach_timebase_info(&tb);
	return ms * 1000000ull * tb.denom / tb.numer;
}

/* On the queue: a PCM pipe that stopped completing gets the device a reselect and a new start,
 * counted in the stats (ozzy-cli shows it) */
static void watch_fired(void *ctx)
{
	static const enum ozzy_pipe_role roles[] = { OZZY_PIPE_PCM_OUT, OZZY_PIPE_PCM_IN };
	struct daemon_device *d = ctx;
	uint64_t now = mach_absolute_time();
	uint8_t dead = 0;
	unsigned int i;
	bool streaming;

	if (d->closing || !d->running || d->sleeping)
		return;
	for (i = 0; i < sizeof(roles) / sizeof(roles[0]) && !dead; i++) {
		uint8_t address = d->engine->pipe[roles[i]].address;
		int slot = daemon_bus_slot(address);

		if (address && d->bus.submitted[slot] && now - d->bus.last_done[slot] > ms_to_abs(WATCH_DEAD_MS))
			dead = address;
	}
	if (!dead || (d->recovered_at && now - d->recovered_at < ms_to_abs(WATCH_RECOVER_S * 1000ull)))
		return;

	d->recovered_at = now;
	ozzy_log("pipe 0x%02x: no completion for %u ms, recovering the device", dead, WATCH_DEAD_MS);
	ozzy_engine_suspend(d->engine);
	streaming = ozzy_engine_start(d->engine);   /* a restart: it selects the interfaces again */
	ozzy_engine_stats_recovery(d->engine, streaming);
	if (streaming) {
		daemon_bus_watch_reset(&d->bus);
		ozzy_log("recovered: streaming");
	} else {
		/* As a failed restart: the HAL drops the device */
		ozzy_log("recovered: start failed");
		ozzy_engine_stop(d->engine);
		d->running = false;
	}
}

static void watch_start(struct daemon_device *d)
{
	d->watch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, d->queue);
	if (!d->watch)
		return;
	dispatch_set_context(d->watch, d);
	dispatch_source_set_event_handler_f(d->watch, watch_fired);
	dispatch_source_set_timer(d->watch, dispatch_time(DISPATCH_TIME_NOW, WATCH_PERIOD_MS * NSEC_PER_MSEC),
				  WATCH_PERIOD_MS * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
	dispatch_resume(d->watch);
}

/* --- Sleep and wake (OzzyKext::setPowerState), on the queue --- */

static void device_sleep(void *ctx)
{
	struct daemon_device *d = ctx;

	if (d->closing || !d->running || d->sleeping)
		return;
	ozzy_log("sleep: suspending (%s)", d->shm_name);
	ozzy_engine_suspend(d->engine);
	d->sleeping = true;
}

/* The interfaces again and a new start: the transfers in flight across the sleep never
 * complete on some devices (the WZ4, #85) */
static void device_wake(void *ctx)
{
	struct daemon_device *d = ctx;

	if (!d->sleeping)
		return;
	d->sleeping = false;
	if (d->closing || !d->running)
		return;
	ozzy_log("wake: starting again (%s)", d->shm_name);
	if (ozzy_engine_start(d->engine)) {   /* a restart: it selects the interfaces again */
		daemon_bus_watch_reset(&d->bus);
	} else {
		ozzy_log("start after wake failed");
		ozzy_engine_stop(d->engine);
		d->running = false;
	}
}

/* The main thread adds and removes devices, so the list holds still here without the lock */
void daemon_devices_sleep(void)
{
	int i;

	for (i = 0; i < OZZY_DAEMON_MAX_DEVICES; i++)
		if (devices[i])
			dispatch_sync_f(devices[i]->queue, devices[i], device_sleep);
}

void daemon_devices_wake(void)
{
	int i;

	for (i = 0; i < OZZY_DAEMON_MAX_DEVICES; i++)
		if (devices[i])
			dispatch_async_f(devices[i]->queue, devices[i], device_wake);
}

/* Everything up to streaming; false if the daemon can't or shouldn't drive it */
static bool device_open(struct daemon_device *d)
{
	struct ozzy_shared_memory *shm;

	if (!usb_create(d))
		return false;
	if (d->model->family != OZZY_FAMILY_USB_MIDI && usb_class_compliant(d->usb)) {
		ozzy_log("%04x:%04x is class compliant (interface 0 is Audio class): leaving it to the class driver",
			 d->model->vid, d->model->pid);
		return false;
	}
	if (!usb_seize(d))
		return false;
	if ((*d->usb)->GetLocationID(d->usb, &d->location_id) != kIOReturnSuccess)
		d->location_id = registry_u32(d->service, CFSTR("locationID"));

	/* The shared memory, as the kext's: wired, as audio is read from it on the real-time threads */
	snprintf(d->shm_name, sizeof(d->shm_name), OZZY_DAEMON_SHM_FORMAT, d->location_id);
	d->shm = shm_create(d->shm_name, sizeof(*d->shm), 0666, &d->shm_size);
	if (!d->shm)
		return false;
	if (mlock(d->shm, d->shm_size) != 0)
		ozzy_log("%s: mlock failed (%d)", d->shm_name, errno);
	shm = d->shm;
	shm->session_id = arc4random() | 1;   /* 0 is no session */
	registry_string(d->service, CFSTR("USB Vendor Name"), shm->manufacturer_name, sizeof(shm->manufacturer_name));
	registry_string(d->service, CFSTR("USB Product Name"), shm->product_name, sizeof(shm->product_name));
	registry_string(d->service, CFSTR("USB Serial Number"), shm->serial_number, sizeof(shm->serial_number));
	ozzy_log("found device: %s %s (S/N: %s) at 0x%08x", shm->manufacturer_name, shm->product_name,
		 shm->serial_number[0] ? shm->serial_number : "none", d->location_id);

	d->queue = dispatch_queue_create("com.ozzy.daemon.device", DISPATCH_QUEUE_SERIAL);
	if (!d->queue || !usb_thread_start(d))
		return false;
	return engine_start(d);
}

void daemon_device_added(io_service_t service)
{
	uint16_t vid = (uint16_t)registry_u32(service, CFSTR("idVendor"));
	uint16_t pid = (uint16_t)registry_u32(service, CFSTR("idProduct"));
	const struct ozzy_device *model = ozzy_device_lookup(vid, pid);
	struct ozzy_daemon_entry entry;
	struct daemon_device *d;
	int slot, i;

	if (!model)
		return;
	for (i = 0; i < OZZY_DAEMON_MAX_DEVICES; i++)
		if (devices[i] && IOObjectIsEqualTo(devices[i]->service, service))
			return;
	for (slot = 0; slot < OZZY_DAEMON_MAX_DEVICES && devices[slot]; slot++)
		;
	ozzy_log("%04x:%04x: %s%s", vid, pid, model->name,
		 model->status == OZZY_STATUS_EXPERIMENTAL ? " (experimental)" : "");
	if (slot == OZZY_DAEMON_MAX_DEVICES) {
		ozzy_log("too many devices, ignoring it");
		return;
	}

	d = calloc(1, sizeof(*d));
	if (!d)
		return;
	IOObjectRetain(service);
	d->service = service;
	d->model = model;
	d->slot = slot;
	IORegistryEntryGetRegistryEntryID(service, &d->registry_id);
	if (!device_open(d)) {
		device_free(d);
		return;
	}

	memset(&entry, 0, sizeof(entry));
	entry.used = 1;
	entry.location_id = d->location_id;
	entry.session_id = d->shm->session_id;
	entry.vendor_id = vid;
	entry.product_id = pid;
	entry.registry_id = d->registry_id;
	/* OzzyHAL only takes devices with audio (the Xone:1D is MIDI only) */
	entry.has_audio = d->shm->audio.input_channels + d->shm->audio.output_channels > 0;
	memcpy(entry.shm_name, d->shm_name, sizeof(entry.shm_name));
	watch_start(d);

	pthread_mutex_lock(&devices_lock);
	devices[slot] = d;
	pthread_mutex_unlock(&devices_lock);
	table_set(slot, &entry);
	ozzy_log("device ready (%s)", d->shm_name);
}

void daemon_device_removed(io_service_t service)
{
	struct daemon_device *d = NULL;
	int i;

	pthread_mutex_lock(&devices_lock);
	for (i = 0; i < OZZY_DAEMON_MAX_DEVICES && !d; i++) {
		if (devices[i] && IOObjectIsEqualTo(devices[i]->service, service)) {
			d = devices[i];
			devices[i] = NULL;
		}
	}
	pthread_mutex_unlock(&devices_lock);
	if (!d)
		return;
	ozzy_log("device disconnected (%s)", d->shm_name);
	table_set(d->slot, NULL);
	device_free(d);
}

void daemon_devices_exit(void)
{
	int i;

	for (i = 0; i < OZZY_DAEMON_MAX_DEVICES; i++) {
		struct daemon_device *d;

		pthread_mutex_lock(&devices_lock);
		d = devices[i];
		devices[i] = NULL;
		pthread_mutex_unlock(&devices_lock);
		if (!d)
			continue;
		if (table)
			table_set(i, NULL);
		device_free(d);
	}
	if (table) {
		ozzy_store_release_u32(&table->magic, 0);
		shm_destroy(OZZY_DAEMON_TABLE_NAME, table, table_size);
		table = NULL;
	}
}

/* --- Calls --- */

/* A call that waits, on the device's queue: the kext's OzzyUserClient::externalMethod */
static int device_method(struct daemon_device *d, uint32_t method, const uint64_t *args, uint32_t arg_count)
{
	enum ozzy_setting_change change;
	uint32_t id;

	switch (method) {
	case OZZY_METHOD_APPLY_SETTING:
		/* Restart with the setting, as the official driver suspends, sends it and resumes; the
		 * same value again restarts too (the handshake re-reads the digital lock). Device
		 * settings only: the HAL applies routes itself. */
		if (arg_count != 2 || args[0] > 0xFFFF || args[1] > 0xFF)
			return kIOReturnBadArgument;
		if (!d->running)
			return kIOReturnNotReady;
		id = (uint32_t)args[0];
		if (id >= OZZY_NUM_SETTINGS || ozzy_setting_descs[id].kind != OZZY_SETTING_DEVICE ||
		    ozzy_engine_change_setting(d->engine, id, (unsigned int)args[1]) == OZZY_SETTING_INVALID)
			return kIOReturnBadArgument;
		return engine_restart(d);
	case OZZY_METHOD_SET_RATE:
		/* Restart at the rate, unless it runs at it already */
		if (arg_count != 1 || args[0] > 0xFFFFFFFF)
			return kIOReturnBadArgument;
		if (!d->running)
			return kIOReturnNotReady;
		change = ozzy_engine_change_rate(d->engine, (uint32_t)args[0]);
		return change == OZZY_SETTING_INVALID ? kIOReturnBadArgument
		       : change == OZZY_SETTING_RESTART ? engine_restart(d) : kIOReturnSuccess;
	default:
		return kIOReturnUnsupported;
	}
}

struct call_job {
	struct daemon_device *device;
	uint32_t method;
	uint32_t arg_count;
	uint64_t args[2];
	mach_port_t reply;
};

static void call_run(void *ctx)
{
	struct call_job *job = ctx;
	struct daemon_device *d = job->device;
	int result = d->closing ? kIOReturnNoDevice : device_method(d, job->method, job->args, job->arg_count);

	daemon_server_reply(job->reply, result);
	free(job);
}

bool daemon_device_call(uint32_t location_id, uint32_t session_id, uint32_t method, const uint64_t *args,
			uint32_t arg_count, mach_port_t reply)
{
	struct daemon_device *d = NULL;
	struct call_job *job;
	int i;

	pthread_mutex_lock(&devices_lock);
	for (i = 0; i < OZZY_DAEMON_MAX_DEVICES && !d; i++)
		if (devices[i] && devices[i]->location_id == location_id && devices[i]->shm->session_id == session_id)
			d = devices[i];
	if (!d) {
		pthread_mutex_unlock(&devices_lock);
		return false;
	}

	if (method == OZZY_METHOD_MIDI_OUT_PENDING) {
		/* At once, as the kext does (no lock on the device; the list's keeps it alive) */
		if (d->running)
			ozzy_engine_midi_out_pending(d->engine);
		pthread_mutex_unlock(&devices_lock);
		if (reply != MACH_PORT_NULL)
			daemon_server_reply(reply, kIOReturnSuccess);
		return true;
	}

	job = calloc(1, sizeof(*job));
	if (!job) {
		pthread_mutex_unlock(&devices_lock);
		daemon_server_reply(reply, kIOReturnNoMemory);
		return true;
	}
	job->device = d;
	job->method = method;
	job->arg_count = arg_count > 2 ? 2 : arg_count;
	memcpy(job->args, args, sizeof(job->args));
	job->reply = reply;
	/* Queued under the lock: device_free's drain comes after it */
	dispatch_async_f(d->queue, job, call_run);
	pthread_mutex_unlock(&devices_lock);
	return true;
}
