#include "OzzyMIDI.h"
#include "../../engine/shared/ozzy_shared_data.h"
#include "../Shared/OzzyLog.h"
#include "../Shared/OzzyKextClient.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>
#include <IOKit/IOKitLib.h>

/*
 * One port per OzzyKext instance (a DB4 and a Xone:1D can be connected at the same time).
 * A port keeps its MIDI device when the hardware goes away (it goes offline), so apps keep
 * their connections when the same device comes back.
 *
 * Threads: everything that changes CoreMIDI devices (create, rename, online/offline, dispose)
 * runs on MIDIServer's main run loop, from Start/Stop and a scan timer. MIDIServer calls Stop
 * there while holding its own lock, and those calls take that lock too, so making them on the
 * poll thread deadlocks Stop's join. The poll thread only drains the input rings and calls
 * MIDIReceived.
 */
#define kMaxPorts 8

// MIDI out that doesn't fit the shared ring waits here until the kext has drained it: the
// device takes 2500 bytes/s and the ring holds 1 KB. 64 KB like the official driver's MIDI
// queue (26 s of MIDI).
#define kPendingSize 65536
#define kPendingMask (kPendingSize - 1)

struct OzzyMidiPort {
	// Connection to one OzzyKext instance; shm is null while disconnected
	uint64_t registryID;
	uint32_t locationID;   // USB port: tells two devices of the same model apart
	io_connect_t connect;
	mach_vm_address_t mapAddress;
	mach_vm_size_t mapSize;
	ozzy_shared_memory* shm;
	uint32_t sessionID;

	// CoreMIDI objects, kept across reconnects
	MIDIDeviceRef device;
	MIDIEndpointRef source;
	char name[64];

	// Bumped on every connect, so the poll thread knows to reset its parser
	uint32_t generation;

	// MIDI out waiting for room in the shared ring, oldest first (gPortLock)
	Byte pending[kPendingSize];
	uint32_t pendingHead;
	uint32_t pendingTail;
	uint32_t dropped;      // bytes lost because pending was full too, reported once it drains
};

/* MIDI input parser, owned by the poll thread */
struct OzzyMidiParser {
	uint32_t generation;
	Byte msg[16];
	int msgIndex;
	int msgExpected;
};

static OzzyMidiPort gPorts[kMaxPorts];
static MIDIDriverRef gDriverRef = nullptr;

// Held around every use of a port's shared memory and pending MIDI out by Send and the poll
// thread, and while the main thread maps or unmaps it. Never held across a CoreMIDI call.
static pthread_mutex_t gPortLock = PTHREAD_MUTEX_INITIALIZER;

static std::atomic<bool> gThreadShouldRun{false};
static pthread_t gPollThread = nullptr;
static CFRunLoopTimerRef gScanTimer = nullptr;

static void SetPortDevice(OzzyMidiPort* port, const ozzy_shared_memory* shm) {
	const char* name = shm->product_name[0] ? shm->product_name : "Ozzy MIDI";
	const char* mfg = shm->manufacturer_name[0] ? shm->manufacturer_name : "Ozzy";
	CFStringRef deviceName = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	CFStringRef vendorName = CFStringCreateWithCString(NULL, mfg, kCFStringEncodingUTF8);

	if (port->device) {
		if (deviceName) MIDIObjectSetStringProperty(port->device, kMIDIPropertyName, deviceName);
		if (vendorName) MIDIObjectSetStringProperty(port->device, kMIDIPropertyManufacturer, vendorName);
	} else {
		LogOzzyMIDI("creating device: '%{public}s'", name);
		OSStatus err = MIDIDeviceCreate(gDriverRef, deviceName, vendorName, deviceName, &port->device);
		if (err == noErr) {
			MIDIEntityRef ent;
			MIDIDeviceAddEntity(port->device, CFSTR("MIDI"), false, 1, 1, &ent);
			port->source = MIDIEntityGetSource(ent, 0);
			MIDIEndpointSetRefCons(MIDIEntityGetDestination(ent, 0), port, nullptr);
			MIDIObjectSetIntegerProperty(port->device, kMIDIPropertyDriverOwner, 1);
		} else {
			LogOzzyMIDIError("failed to create device: %{public}d", (int)err);
			port->device = 0;
		}
	}
	if (port->device) MIDIObjectSetIntegerProperty(port->device, kMIDIPropertyOffline, 0);
	strlcpy(port->name, name, sizeof(port->name));

	if (deviceName) CFRelease(deviceName);
	if (vendorName) CFRelease(vendorName);
}

/* Close the port's kext connection; no CoreMIDI calls */
static void UnmapPort(OzzyMidiPort* port) {
	pthread_mutex_lock(&gPortLock);
	IOConnectUnmapMemory(port->connect, 0, mach_task_self(), port->mapAddress);
	IOServiceClose(port->connect);
	port->connect = IO_OBJECT_NULL;
	port->mapAddress = 0;
	port->mapSize = 0;
	port->shm = nullptr;
	port->registryID = 0;
	port->pendingHead = port->pendingTail = 0;
	pthread_mutex_unlock(&gPortLock);
}

static void DisconnectPort(OzzyMidiPort* port) {
	if (!port->shm) return;
	UnmapPort(port);
	if (port->device) MIDIObjectSetIntegerProperty(port->device, kMIDIPropertyOffline, 1);
	LogOzzyMIDI("'%{public}s' disconnected", port->name);
}

static bool PortIsConnected(uint64_t registryID) {
	for (int i = 0; i < kMaxPorts; i++)
		if (gPorts[i].shm && gPorts[i].registryID == registryID) return true;
	return false;
}

/* A free port for a device called @name on USB port @locationID: preferably the one it had
 * before, else an unused one, else any disconnected port (its device gets renamed). */
static OzzyMidiPort* PortForDevice(const char* name, uint32_t locationID) {
	for (int i = 0; i < kMaxPorts; i++)
		if (!gPorts[i].shm && gPorts[i].device && gPorts[i].locationID == locationID &&
		    strcmp(gPorts[i].name, name) == 0) return &gPorts[i];
	for (int i = 0; i < kMaxPorts; i++)
		if (!gPorts[i].shm && !gPorts[i].device) return &gPorts[i];
	for (int i = 0; i < kMaxPorts; i++)
		if (!gPorts[i].shm) return &gPorts[i];
	return nullptr;
}

static void ConnectService(io_service_t service, uint64_t registryID) {
	io_connect_t connect = IO_OBJECT_NULL;
	mach_vm_address_t address = 0;
	mach_vm_size_t size = 0;

	if (IOServiceOpen(service, mach_task_self(), 0, &connect) != KERN_SUCCESS) return;
	kern_return_t kr = IOConnectMapMemory(connect, 0, mach_task_self(), &address, &size, kIOMapAnywhere);
	if (kr != KERN_SUCCESS) {
		LogOzzyMIDIError("failed to map kext memory (0x%{public}x)", kr);
		IOServiceClose(connect);
		return;
	}

	ozzy_shared_memory* shm = (ozzy_shared_memory*)address;
	OzzyMidiPort* port = nullptr;
	if (shm->magic != OZZY_MAGIC || shm->version != OZZY_SHARED_VERSION) {
		LogOzzyMIDIError("SHM magic/version mismatch (version %{public}u, expected %{public}u): reinstall Ozzy",
		                 shm->version, OZZY_SHARED_VERSION);
	} else if (!shm->audio.hardware_present.load()) {
		// Not started yet; try again on the next scan
	} else {
		port = PortForDevice(shm->product_name[0] ? shm->product_name : "Ozzy MIDI", OzzyUSBLocationID(service));
		if (!port) LogOzzyMIDIError("too many devices");
	}
	if (!port) {
		IOConnectUnmapMemory(connect, 0, mach_task_self(), address);
		IOServiceClose(connect);
		return;
	}

	SetPortDevice(port, shm);

	pthread_mutex_lock(&gPortLock);
	port->registryID = registryID;
	port->locationID = OzzyUSBLocationID(service);
	port->connect = connect;
	port->mapAddress = address;
	port->mapSize = size;
	port->sessionID = shm->session_id;
	port->generation++;
	port->shm = shm;
	pthread_mutex_unlock(&gPortLock);
	LogOzzyMIDI("connected to '%{public}s' (session: 0x%{public}08X)", port->name, port->sessionID);
}

/* Connect to OzzyKext instances we don't have yet and drop ports whose device went away */
static void ScanDevices() {
	for (int i = 0; i < kMaxPorts; i++) {
		OzzyMidiPort* port = &gPorts[i];
		if (!port->shm) continue;
		if (port->shm->magic != OZZY_MAGIC || port->shm->session_id != port->sessionID ||
		    !port->shm->audio.hardware_present.load(std::memory_order_relaxed)) {
			DisconnectPort(port);
		}
	}

	io_iterator_t it = IO_OBJECT_NULL;
	if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("OzzyKext"), &it) != KERN_SUCCESS) return;
	io_service_t service;
	while ((service = IOIteratorNext(it))) {
		uint64_t registryID = 0;
		IORegistryEntryGetRegistryEntryID(service, &registryID);
		if (!PortIsConnected(registryID)) ConnectService(service, registryID);
		IOObjectRelease(service);
	}
	IOObjectRelease(it);
}

static int GetExpectedDataLength(uint8_t status) {
	if (status >= 0xF0) {
		switch (status) {
			case 0xF1: return 1;
			case 0xF2: return 2;
			case 0xF3: return 1;
			default: return 0;
		}
	}
	uint8_t high = status & 0xF0;
	return (high == 0xC0 || high == 0xD0) ? 1 : 2;
}

static void SendToSource(MIDIEndpointRef source, const Byte* data, int length) {
	MIDIPacketList packetList;
	MIDIPacket* curPacket = MIDIPacketListInit(&packetList);
	curPacket = MIDIPacketListAdd(&packetList, sizeof(packetList), curPacket, mach_absolute_time(), length, data);
	if (curPacket) MIDIReceived(source, &packetList);
}

/* Drain the port's midiIn ring into CoreMIDI. Returns true if there was data. */
static bool DrainInput(OzzyMidiPort* port, OzzyMidiParser* parser) {
	Byte bytes[OZZY_MIDI_RING_SIZE];
	uint32_t count = 0;

	// Copy the ring out under the lock, so the main thread can't unmap it meanwhile
	pthread_mutex_lock(&gPortLock);
	ozzy_shared_memory* shm = port->shm;
	MIDIEndpointRef source = port->source;
	uint32_t generation = port->generation;
	if (shm) {
		uint32_t r = std::atomic_load_explicit(&shm->midi_in.read_index, std::memory_order_relaxed);
		uint32_t w = std::atomic_load_explicit(&shm->midi_in.write_index, std::memory_order_acquire);
		while (r != w) {
			bytes[count++] = shm->midi_in.buffer[r];
			r = (r + 1) & OZZY_MIDI_RING_MASK;
		}
		std::atomic_store_explicit(&shm->midi_in.read_index, r, std::memory_order_release);
	}
	pthread_mutex_unlock(&gPortLock);

	if (parser->generation != generation) {
		*parser = OzzyMidiParser{};
		parser->generation = generation;
	}
	if (!count || !source) return count != 0;

	for (uint32_t i = 0; i < count; i++) {
		uint8_t byte = bytes[i];

		if (byte >= 0xF8) {
			SendToSource(source, &byte, 1);
			continue;
		}

		if (byte >= 0x80) {
			parser->msg[0] = byte;
			parser->msgIndex = 1;
			parser->msgExpected = 1 + GetExpectedDataLength(byte);

			if (byte == 0xF6 || byte == 0xF7 || parser->msgExpected == 1) {
				SendToSource(source, parser->msg, 1);
				parser->msgIndex = 0; parser->msgExpected = 0;
			}
			continue;
		}

		if (parser->msgIndex > 0 && parser->msgIndex < 16) {
			parser->msg[parser->msgIndex++] = byte;
			if (parser->msgIndex == parser->msgExpected) {
				SendToSource(source, parser->msg, parser->msgIndex);
				parser->msgIndex = 0; parser->msgExpected = 0;
			}
		}
	}
	return true;
}

/*
 * Queue MIDI out for the kext: pending bytes first, then @data, into the shared ring as far as
 * it has room; the rest waits in pending. Call with gPortLock held and the port connected.
 * Returns true if anything reached the ring.
 */
static bool QueueMidiOut(OzzyMidiPort* port, const Byte* data, uint32_t length) {
	ozzy_shared_memory* shm = port->shm;
	uint32_t w = std::atomic_load_explicit(&shm->midi_out.write_index, std::memory_order_relaxed);
	uint32_t r = std::atomic_load_explicit(&shm->midi_out.read_index, std::memory_order_acquire);
	uint32_t space = (r - w - 1) & OZZY_MIDI_RING_MASK;
	uint32_t moved = 0, i = 0;

	while (space && port->pendingHead != port->pendingTail) {
		shm->midi_out.buffer[w] = port->pending[port->pendingHead];
		port->pendingHead = (port->pendingHead + 1) & kPendingMask;
		w = (w + 1) & OZZY_MIDI_RING_MASK;
		space--; moved++;
	}
	if (port->pendingHead == port->pendingTail) {
		for (; space && i < length; i++, space--, moved++) {
			shm->midi_out.buffer[w] = data[i];
			w = (w + 1) & OZZY_MIDI_RING_MASK;
		}
	}
	std::atomic_store_explicit(&shm->midi_out.write_index, w, std::memory_order_release);

	for (; i < length; i++) {
		uint32_t next = (port->pendingTail + 1) & kPendingMask;
		if (next == port->pendingHead) {
			port->dropped += length - i;
			break;
		}
		port->pending[port->pendingTail] = data[i];
		port->pendingTail = next;
	}
	return moved != 0;
}

/* Move waiting MIDI out into the shared ring as the kext drains it. Returns true if any moved. */
static bool FlushMidiOut(OzzyMidiPort* port) {
	uint32_t dropped = 0;

	pthread_mutex_lock(&gPortLock);
	bool moved = port->shm && port->pendingHead != port->pendingTail && QueueMidiOut(port, nullptr, 0);
	if (moved) IOConnectCallScalarMethod(port->connect, OZZY_METHOD_MIDI_OUT_PENDING, NULL, 0, NULL, NULL);
	if (port->pendingHead == port->pendingTail) {
		dropped = port->dropped;
		port->dropped = 0;
	}
	pthread_mutex_unlock(&gPortLock);

	if (dropped) LogOzzyMIDIError("'%{public}s': MIDI out queue drained, %{public}u bytes were dropped", port->name, dropped);
	return moved;
}

static void* MidiInputPollThread(void* arg) {
	LogOzzyMIDI("poll thread started");
	OzzyMidiParser parsers[kMaxPorts] = {};

	while (gThreadShouldRun.load()) {
		bool didWork = false;
		for (int i = 0; i < kMaxPorts; i++) {
			didWork |= DrainInput(&gPorts[i], &parsers[i]);
			didWork |= FlushMidiOut(&gPorts[i]);
		}
		if (!didWork) usleep(500);
	}
	return NULL;
}

static void ScanTimerFired(CFRunLoopTimerRef, void*) {
	ScanDevices();
}

static OSStatus FindDevices(MIDIDriverRef self, MIDIDeviceListRef devList) {
	for (int i = 0; i < kMaxPorts; i++) {
		if (gPorts[i].device) MIDIDeviceListAddDevice(devList, gPorts[i].device);
	}
	return noErr;
}
static OSStatus Configure(MIDIDriverRef, MIDIDeviceRef) { return noErr; }
static OSStatus EnableSource(MIDIDriverRef, MIDIEndpointRef, Boolean) { return noErr; }
static OSStatus Flush(MIDIDriverRef, MIDIEndpointRef, void*, void*) { return noErr; }
static OSStatus Monitor(MIDIDriverRef, MIDIEndpointRef, const MIDIPacketList*) { return noErr; }

static OSStatus Start(MIDIDriverRef self, MIDIDeviceListRef devList) {
	gDriverRef = self;
	ScanDevices();
	if (!gScanTimer) {
		// Look for new and departed devices every 50 ms
		gScanTimer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.05, 0.05, 0, 0, ScanTimerFired, NULL);
		CFRunLoopAddTimer(CFRunLoopGetMain(), gScanTimer, kCFRunLoopCommonModes);
	}
	if (!gThreadShouldRun.load()) {
		gThreadShouldRun.store(true);
		pthread_create(&gPollThread, NULL, MidiInputPollThread, NULL);
	}
	return noErr;
}

static OSStatus Stop(MIDIDriverRef self) {
	if (gScanTimer) {
		CFRunLoopTimerInvalidate(gScanTimer);
		CFRelease(gScanTimer);
		gScanTimer = nullptr;
	}
	if (gThreadShouldRun.load()) {
		gThreadShouldRun.store(false);
		pthread_join(gPollThread, NULL);
		gPollThread = nullptr;
	}
	// Only close the kext connections. MIDIServer stops its drivers when it shuts down, and
	// changing or disposing devices that are in the setup then crashes it (in whichever
	// driver it stops next); the devices stay in the setup as they are.
	for (int i = 0; i < kMaxPorts; i++) {
		if (gPorts[i].shm) UnmapPort(&gPorts[i]);
	}
	return noErr;
}

/* Queue the bytes for the kext (QueueMidiOut), then tell it. Ploytec devices pick them up
 * with the next PCM packet anyway; the Xone:1D sends only when told. */
static OSStatus Send(MIDIDriverRef self, const MIDIPacketList *pktlist, void *destRefCon1, void *destRefCon2) {
	OzzyMidiPort* port = (OzzyMidiPort*)destRefCon1;
	if (!port) return kMIDINoConnection;

	pthread_mutex_lock(&gPortLock);
	if (!port->shm) {
		pthread_mutex_unlock(&gPortLock);
		return kMIDINoConnection;
	}

	uint32_t droppedBefore = port->dropped;
	const MIDIPacket *packet = &pktlist->packet[0];
	for (unsigned int i = 0; i < pktlist->numPackets; ++i) {
		QueueMidiOut(port, packet->data, packet->length);
		packet = MIDIPacketNext(packet);
	}
	IOConnectCallScalarMethod(port->connect, OZZY_METHOD_MIDI_OUT_PENDING, NULL, 0, NULL, NULL);
	bool startedDropping = droppedBefore == 0 && port->dropped != 0;
	pthread_mutex_unlock(&gPortLock);

	if (startedDropping) LogOzzyMIDIError("'%{public}s': MIDI out queue full (64 KB), dropping", port->name);
	return noErr;
}

static HRESULT QueryInterface(void* self, REFIID iid, void** out) {
	CFUUIDBytes interfaceBytes = CFUUIDGetUUIDBytes(kOzzyInterfaceUUID);
	if (memcmp(&iid, &interfaceBytes, sizeof(REFIID)) == 0) {
		*out = self;
		((OzzyMIDIDriver*)self)->_refCount++;
		return 0;
	}
	*out = NULL;
	return 0x80004002;
}
static ULONG AddRef(void* self) { return ++((OzzyMIDIDriver*)self)->_refCount; }
static ULONG Release(void* self) {
	OzzyMIDIDriver* driver = (OzzyMIDIDriver*)self;
	if (--driver->_refCount == 0) { free(driver); return 0; }
	return driver->_refCount;
}

static MIDIDriverInterface gOzzyInterfaceTable = {
	NULL, QueryInterface, AddRef, Release, FindDevices, Start, Stop, Configure, Send, EnableSource, Flush, Monitor
};

extern "C" {
	__attribute__((visibility("default")))
	void* OzzyMIDIFactory(CFAllocatorRef allocator, CFUUIDRef typeID) {
		if (CFEqual(typeID, kOzzyMIDIDriverTypeID)) {
			OzzyMIDIDriver* driver = (OzzyMIDIDriver*)calloc(1, sizeof(OzzyMIDIDriver));
			driver->_vtbl = &gOzzyInterfaceTable;
			driver->_refCount = 1;
			return driver;
		}
		return NULL;
	}
}
