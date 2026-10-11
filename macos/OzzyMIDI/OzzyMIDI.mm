#include "OzzyMIDI.h"
#include "../Shared/OzzySharedData.h"
#include "../Shared/OzzyLog.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>
#include <IOKit/IOKitLib.h>
#include <os/lock.h>

/*
 * One port per OzzyKext instance (a DB4 and a Xone:1D can be connected at the same time).
 * A port keeps its MIDI device when the hardware goes away (it goes offline), so apps keep
 * their connections when the same device comes back.
 */
#define kMaxPorts 8

struct OzzyMidiPort {
	// Connection to one OzzyKext instance; shm is null while disconnected
	uint64_t registryID;
	io_connect_t connect;
	mach_vm_address_t mapAddress;
	mach_vm_size_t mapSize;
	OzzySharedMemory* shm;
	uint32_t sessionID;

	// CoreMIDI objects, kept across reconnects
	MIDIDeviceRef device;
	MIDIEndpointRef source;
	char name[64];

	// MIDI input parser state
	Byte msg[16];
	int msgIndex;
	int msgExpected;
};

static OzzyMidiPort gPorts[kMaxPorts];
static MIDIDriverRef gDriverRef = nullptr;

// Held by Send while it writes to a port's shared memory, and by Disconnect while unmapping it
static os_unfair_lock gPortLock = OS_UNFAIR_LOCK_INIT;

static std::atomic<bool> gThreadShouldRun{false};
static pthread_t gPollThread = nullptr;

static void SetPortDevice(OzzyMidiPort* port) {
	const char* name = port->shm->productName[0] ? port->shm->productName : "Ozzy MIDI";
	const char* mfg = port->shm->manufacturerName[0] ? port->shm->manufacturerName : "Ozzy";
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

static void DisconnectPort(OzzyMidiPort* port) {
	if (!port->shm) return;
	os_unfair_lock_lock(&gPortLock);
	IOConnectUnmapMemory(port->connect, 0, mach_task_self(), port->mapAddress);
	IOServiceClose(port->connect);
	port->connect = IO_OBJECT_NULL;
	port->mapAddress = 0;
	port->mapSize = 0;
	port->shm = nullptr;
	port->registryID = 0;
	os_unfair_lock_unlock(&gPortLock);

	if (port->device) MIDIObjectSetIntegerProperty(port->device, kMIDIPropertyOffline, 1);
	LogOzzyMIDI("'%{public}s' disconnected", port->name);
}

static bool PortIsConnected(uint64_t registryID) {
	for (int i = 0; i < kMaxPorts; i++)
		if (gPorts[i].shm && gPorts[i].registryID == registryID) return true;
	return false;
}

/* A free port for a device called @name: preferably the one it had before, else an unused
 * one, else any disconnected port (its device gets renamed). */
static OzzyMidiPort* PortForName(const char* name) {
	for (int i = 0; i < kMaxPorts; i++)
		if (!gPorts[i].shm && gPorts[i].device && strcmp(gPorts[i].name, name) == 0) return &gPorts[i];
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

	OzzySharedMemory* shm = (OzzySharedMemory*)address;
	OzzyMidiPort* port = nullptr;
	if (shm->magic != kOzzyMagic || shm->version != kOzzySharedVersion) {
		LogOzzyMIDIError("SHM magic/version mismatch (version %{public}u, expected %{public}u): reinstall Ozzy",
		                 shm->version, kOzzySharedVersion);
	} else if (!shm->audio.hardwarePresent.load()) {
		// Not started yet; try again on the next scan
	} else {
		port = PortForName(shm->productName[0] ? shm->productName : "Ozzy MIDI");
		if (!port) LogOzzyMIDIError("too many devices");
	}
	if (!port) {
		IOConnectUnmapMemory(connect, 0, mach_task_self(), address);
		IOServiceClose(connect);
		return;
	}

	port->registryID = registryID;
	port->connect = connect;
	port->mapAddress = address;
	port->mapSize = size;
	port->sessionID = shm->sessionID;
	port->msgIndex = 0;
	port->msgExpected = 0;
	port->shm = shm;
	SetPortDevice(port);
	LogOzzyMIDI("connected to '%{public}s' (session: 0x%{public}08X)", port->name, port->sessionID);
}

/* Connect to OzzyKext instances we don't have yet and drop ports whose device went away */
static void ScanDevices() {
	for (int i = 0; i < kMaxPorts; i++) {
		OzzyMidiPort* port = &gPorts[i];
		if (!port->shm) continue;
		if (port->shm->magic != kOzzyMagic || port->shm->sessionID != port->sessionID ||
		    !port->shm->audio.hardwarePresent.load(std::memory_order_relaxed)) {
			DisconnectPort(port);
		}
	}

	io_iterator_t it = IO_OBJECT_NULL;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("OzzyKext"), &it) != KERN_SUCCESS) return;
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

static void SendToSource(OzzyMidiPort* port, const Byte* data, int length) {
	if (!port->source) return;
	MIDIPacketList packetList;
	MIDIPacket* curPacket = MIDIPacketListInit(&packetList);
	curPacket = MIDIPacketListAdd(&packetList, sizeof(packetList), curPacket, mach_absolute_time(), length, data);
	if (curPacket) MIDIReceived(port->source, &packetList);
}

/* Drain the port's midiIn ring into CoreMIDI. Returns true if there was data. */
static bool DrainInput(OzzyMidiPort* port) {
	OzzySharedMemory* shm = port->shm;
	uint32_t r = std::atomic_load_explicit(&shm->midiIn.readIndex, std::memory_order_relaxed);
	uint32_t w = std::atomic_load_explicit(&shm->midiIn.writeIndex, std::memory_order_acquire);
	if (r == w) return false;

	while (r != w) {
		uint8_t byte = shm->midiIn.buffer[r];
		r = (r + 1) & kOzzyMidiRingMask;

		if (byte >= 0xF8) {
			SendToSource(port, &byte, 1);
			continue;
		}

		if (byte >= 0x80) {
			port->msg[0] = byte;
			port->msgIndex = 1;
			port->msgExpected = 1 + GetExpectedDataLength(byte);

			if (byte == 0xF6 || byte == 0xF7 || port->msgExpected == 1) {
				SendToSource(port, port->msg, 1);
				port->msgIndex = 0; port->msgExpected = 0;
			}
			continue;
		}

		if (port->msgIndex > 0 && port->msgIndex < 16) {
			port->msg[port->msgIndex++] = byte;
			if (port->msgIndex == port->msgExpected) {
				SendToSource(port, port->msg, port->msgIndex);
				port->msgIndex = 0; port->msgExpected = 0;
			}
		}
	}

	std::atomic_store_explicit(&shm->midiIn.readIndex, r, std::memory_order_release);
	return true;
}

static void* MidiInputPollThread(void* arg) {
	LogOzzyMIDI("poll thread started");
	int scanCounter = 0;

	while (gThreadShouldRun.load()) {
		// Scan for new and departed devices about every 50 ms
		if (--scanCounter <= 0) {
			ScanDevices();
			scanCounter = 100;
		}

		bool didWork = false;
		for (int i = 0; i < kMaxPorts; i++) {
			if (gPorts[i].shm) didWork |= DrainInput(&gPorts[i]);
		}
		if (!didWork) usleep(500);
	}

	for (int i = 0; i < kMaxPorts; i++) DisconnectPort(&gPorts[i]);
	return NULL;
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
	if (!gThreadShouldRun.load()) {
		gThreadShouldRun.store(true);
		pthread_create(&gPollThread, NULL, MidiInputPollThread, NULL);
	}
	return noErr;
}

static OSStatus Stop(MIDIDriverRef self) {
	if (gThreadShouldRun.load()) {
		gThreadShouldRun.store(false);
		pthread_join(gPollThread, NULL);
		gPollThread = nullptr;
	}
	for (int i = 0; i < kMaxPorts; i++) {
		DisconnectPort(&gPorts[i]);
		if (gPorts[i].device) MIDIDeviceDispose(gPorts[i].device);
		gPorts[i] = OzzyMidiPort{};
	}
	gDriverRef = nullptr;
	return noErr;
}

/* Queue the bytes in the port's midiOut ring, then tell the kext. Ploytec devices pick them
 * up with the next PCM packet anyway; the Xone:1D sends only when told. */
static OSStatus Send(MIDIDriverRef self, const MIDIPacketList *pktlist, void *destRefCon1, void *destRefCon2) {
	OzzyMidiPort* port = (OzzyMidiPort*)destRefCon1;
	if (!port) return kMIDINoConnection;

	os_unfair_lock_lock(&gPortLock);
	OzzySharedMemory* shm = port->shm;
	if (!shm) {
		os_unfair_lock_unlock(&gPortLock);
		return kMIDINoConnection;
	}

	const MIDIPacket *packet = &pktlist->packet[0];
	for (unsigned int i = 0; i < pktlist->numPackets; ++i) {
		for (unsigned int j = 0; j < packet->length; ++j) {
			uint32_t w = std::atomic_load_explicit(&shm->midiOut.writeIndex, std::memory_order_relaxed);
			uint32_t r = std::atomic_load_explicit(&shm->midiOut.readIndex, std::memory_order_acquire);
			uint32_t nextW = (w + 1) & kOzzyMidiRingMask;
			if (nextW != r) {
				shm->midiOut.buffer[w] = packet->data[j];
				std::atomic_store_explicit(&shm->midiOut.writeIndex, nextW, std::memory_order_release);
			}
		}
		packet = MIDIPacketNext(packet);
	}
	IOConnectCallScalarMethod(port->connect, kOzzyMethodMidiOutPending, NULL, 0, NULL, NULL);
	os_unfair_lock_unlock(&gPortLock);
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
