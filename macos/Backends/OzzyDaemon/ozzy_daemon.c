/* SPDX-License-Identifier: MIT */
/*
 * OzzyDaemon: Ozzy's userspace driver for macOS, an opt-in alternative to OzzyKext for systems
 * that can't or won't allow kernel extensions. It drives the same devices with the same engine,
 * and OzzyHAL and OzzyMIDI talk to it instead of the kext (macos/Shared/ozzy_daemon.h).
 * User space can't get the priority the USB side needs, so streams recover more often under
 * load than with the kext; ozzy-cli's stream stats show it.
 *
 * A root launchd job (com.ozzy.daemon.plist), never more than one: matching notifications for
 * USB devices on the main thread's run loop, the clients' calls on daemon_server's thread, each
 * device's USB completions on its own (daemon_device.c).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "daemon_device.h"
#include "daemon_server.h"
#include "../../../engine/shared/ozzy_log.h"
#include "../../../common/ozzy_version.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <dispatch/dispatch.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

static void added(void *ctx, io_iterator_t it)
{
	io_service_t service;

	(void)ctx;
	while ((service = IOIteratorNext(it))) {
		daemon_device_added(service);
		IOObjectRelease(service);
	}
}

static void removed(void *ctx, io_iterator_t it)
{
	io_service_t service;

	(void)ctx;
	while ((service = IOIteratorNext(it))) {
		daemon_device_removed(service);
		IOObjectRelease(service);
	}
}

/* Every USB device, arriving and leaving; daemon_device picks the listed ones */
static bool watch_devices(IONotificationPortRef port)
{
	static io_iterator_t added_it, removed_it;
	CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");

	if (!match)
		return false;
	CFRetain(match);   /* each call consumes a reference */
	if (IOServiceAddMatchingNotification(port, kIOMatchedNotification, match, added, NULL, &added_it) != KERN_SUCCESS ||
	    IOServiceAddMatchingNotification(port, kIOTerminatedNotification, match, removed, NULL, &removed_it) !=
		    KERN_SUCCESS) {
		ozzy_log("can't watch for USB devices");
		return false;
	}
	/* The ones already there; arming the iterators */
	added(NULL, added_it);
	removed(NULL, removed_it);
	return true;
}

/* --- System sleep and wake --- */

static io_connect_t power_root;

/* Suspend the devices before agreeing to sleep; start them again once the system is up */
static void power_changed(void *ctx, io_service_t service, natural_t type, void *arg)
{
	(void)ctx;
	(void)service;
	switch (type) {
	case kIOMessageCanSystemSleep:
		IOAllowPowerChange(power_root, (long)arg);
		break;
	case kIOMessageSystemWillSleep:
		daemon_devices_sleep();
		IOAllowPowerChange(power_root, (long)arg);
		break;
	case kIOMessageSystemHasPoweredOn:
		daemon_devices_wake();
		break;
	default:
		break;
	}
}

static IONotificationPortRef watch_power(void)
{
	IONotificationPortRef port = NULL;
	io_object_t notifier;

	power_root = IORegisterForSystemPower(NULL, &port, power_changed, &notifier);
	if (!power_root) {
		ozzy_log("no sleep/wake notifications: devices keep streaming across a sleep");
		return NULL;
	}
	CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);
	return port;
}

static void on_signal(void *ctx)
{
	(void)ctx;
	ozzy_log("stopping");
	CFRunLoopStop(CFRunLoopGetMain());
}

static dispatch_source_t catch_signal(int sig)
{
	dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sig, 0,
						       dispatch_get_main_queue());

	signal(sig, SIG_IGN);
	if (src) {
		dispatch_source_set_event_handler_f(src, on_signal);
		dispatch_resume(src);
	}
	return src;
}

int main(void)
{
	IONotificationPortRef port;
	dispatch_source_t term, intr;

	ozzy_log("start (build %s)", OZZY_GIT_VERSION);
	if (geteuid() != 0) {
		ozzy_log("must run as root (it seizes USB devices and publishes their shared memory)");
		return 1;
	}

	/* launchd stops it with SIGTERM: the devices are let go and the shared memory goes */
	term = catch_signal(SIGTERM);
	intr = catch_signal(SIGINT);

	if (!daemon_devices_init())
		return 1;
	daemon_server_start();

	port = IONotificationPortCreate(MACH_PORT_NULL);   /* the default main port, also before macOS 12 */
	if (!port) {
		daemon_devices_exit();
		return 1;
	}
	CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);
	if (!watch_devices(port)) {
		daemon_devices_exit();
		return 1;
	}
	watch_power();

	CFRunLoopRun();

	daemon_devices_exit();
	IONotificationPortDestroy(port);
	if (term)
		dispatch_release(term);
	if (intr)
		dispatch_release(intr);
	ozzy_log("stopped");
	return 0;
}
