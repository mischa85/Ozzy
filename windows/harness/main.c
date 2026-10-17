// ozzy-harness: runs an Ozzy engine against a device bound to WinUSB, for protocol work on
// Windows without loading a kernel driver. It plays the HAL's part of the shared memory: a
// test tone in the output ring, the input ring to a WAV file, MIDI in to the console.
//
//   ozzy-harness [-l] [-d N] [-s seconds] [-r capture.wav] [-q]
//     -l  list the supported devices found and what driver they use
//     -d  the Nth of them (default 0)
//     -s  stop after this many seconds (default: Ctrl+C)
//     -r  record the input to a 24-bit WAV file
//     -q  silence instead of the test tone

#include "WinUsbBus.h"
#include "../../engine/core/ozzy_engine.h"
#include "../../common/devices/ploytec/ploytec_protocol.h"
#include "../../engine/shared/ozzy_shared_data.h"
#include "../../common/devices/ploytec/ploytec_codec.h"
#include "../../common/ozzy_version.h"
#include <setupapi.h>
#include <objbase.h>
#include <atomic>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

static std::atomic<bool> gQuit{false};

static BOOL WINAPI OnCtrl(DWORD) {
    gQuit = true;
    return TRUE;
}

struct FoundDevice {
    const struct ozzy_device* model;
    std::wstring service;   // the bound driver, e.g. WinUSB
    std::wstring path;      // WinUSB device interface path, empty if there is none
};

/* The device interface path of device @dev, from the interface GUID its WinUSB INF registered
 * (Zadig writes DeviceInterfaceGUIDs; INFs written by hand often DeviceInterfaceGUID) */
static std::wstring WinUsbPath(HDEVINFO set, SP_DEVINFO_DATA* dev) {
    wchar_t guidText[128] = {};
    HKEY key = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) return L"";
    DWORD size = sizeof(guidText) - sizeof(wchar_t);
    bool ok = RegQueryValueExW(key, L"DeviceInterfaceGUIDs", nullptr, nullptr, (BYTE*)guidText, &size) == ERROR_SUCCESS;
    size = sizeof(guidText) - sizeof(wchar_t);
    if (!ok) ok = RegQueryValueExW(key, L"DeviceInterfaceGUID", nullptr, nullptr, (BYTE*)guidText, &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    GUID guid;
    if (!ok || CLSIDFromString(guidText, &guid) != NOERROR) return L"";

    // With DIGCF_DEVICEINTERFACE the enumerator can be a device instance ID: only this device
    wchar_t instance[512];
    if (!SetupDiGetDeviceInstanceIdW(set, dev, instance, 512, nullptr)) return L"";
    HDEVINFO ifs = SetupDiGetClassDevsW(&guid, instance, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (ifs == INVALID_HANDLE_VALUE) return L"";
    std::wstring path;
    SP_DEVICE_INTERFACE_DATA data = { sizeof(data) };
    if (SetupDiEnumDeviceInterfaces(ifs, nullptr, &guid, 0, &data)) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(ifs, &data, nullptr, 0, &needed, nullptr);
        std::vector<BYTE> buf(needed);
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (needed && SetupDiGetDeviceInterfaceDetailW(ifs, &data, detail, needed, nullptr, nullptr))
            path = detail->DevicePath;
    }
    SetupDiDestroyDeviceInfoList(ifs);
    return path;
}

/* Present USB devices that are in common/ozzy_devices.def (whole devices, not interfaces) */
static std::vector<FoundDevice> FindDevices() {
    std::vector<FoundDevice> found;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return found;
    SP_DEVINFO_DATA dev = { sizeof(dev) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        wchar_t ids[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, (BYTE*)ids,
                                               sizeof(ids) - 2 * sizeof(wchar_t), nullptr))
            continue;
        unsigned int vid, pid;
        if (swscanf_s(ids, L"USB\\VID_%4x&PID_%4x", &vid, &pid) != 2 || wcsstr(ids, L"&MI_")) continue;
        const struct ozzy_device* model = ozzy_device_lookup((uint16_t)vid, (uint16_t)pid);
        if (!model) continue;

        wchar_t service[128] = {};
        SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_SERVICE, nullptr, (BYTE*)service,
                                          sizeof(service) - sizeof(wchar_t), nullptr);
        found.push_back({ model, service, _wcsicmp(service, L"WinUSB") == 0 ? WinUsbPath(set, &dev) : L"" });
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// --- 24-bit WAV writer (sizes patched on close) ---
struct Wav {
    FILE* f = nullptr;
    uint32_t channels = 0, bytes = 0;

    bool Open(const char* name, uint32_t rate, uint32_t ch) {
        if (fopen_s(&f, name, "wb") != 0) return false;
        channels = ch;
        uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0 };
        uint32_t blockAlign = ch * 3, byteRate = rate * blockAlign;
        h[22] = (uint8_t)ch;
        memcpy(h + 24, &rate, 4);
        memcpy(h + 28, &byteRate, 4);
        h[32] = (uint8_t)blockAlign;
        h[34] = 24;
        memcpy(h + 36, "data", 4);
        fwrite(h, 1, sizeof(h), f);
        return true;
    }
    void Write(const uint8_t* s24, uint32_t frames) {
        bytes += (uint32_t)fwrite(s24, 1, frames * channels * 3, f);
    }
    void Close() {
        if (!f) return;
        uint32_t riff = 36 + bytes;
        fseek(f, 4, SEEK_SET);
        fwrite(&riff, 4, 1, f);
        fseek(f, 40, SEEK_SET);
        fwrite(&bytes, 4, 1, f);
        fclose(f);
        f = nullptr;
    }
};

static const uint32_t kRingFrames = OZZY_NUM_PACKETS * PLOYTEC_FRAMES_PER_PKT;

/* A -18 dBFS sine on outputs 1 and 2 with a whole number of cycles in the output ring, so the
 * engine can loop the ring forever without a position from us. Written with the float frame
 * codec into the frames of each logical packet; the MIDI and sync bytes stay as Start() set them. */
static double FillTone(ozzy_shared_memory* shm, uint32_t channels, uint32_t rate) {
    ploytec_frame_f32_enc_fn encode = ploytec_frame_encoder_f32(channels);
    if (!encode) return 0;
    enum ploytec_out_layout layout = (shm->audio.device_flags & OZZY_DEVICE_FLAG_PLOYTEC_BULK) ? PLOYTEC_OUT_LAYOUT_BULK
                                                                                            : PLOYTEC_OUT_LAYOUT_INT;
    const uint32_t cycles = 48;
    float frame[16] = {};
    for (uint32_t f = 0; f < kRingFrames; f++) {
        float v = 0.125f * (float)sin(2.0 * 3.14159265358979 * cycles * f / kRingFrames);
        frame[0] = frame[1] = v;
        encode(shm->audio.output_buffer + (f / PLOYTEC_FRAMES_PER_PKT) * OZZY_MAX_PACKET_SIZE +
               ploytec_out_frame_offset(layout, f % PLOYTEC_FRAMES_PER_PKT), frame);
    }
    return (double)cycles * rate / kRingFrames;
}

/* The latest timestamp anchor (seqlock, writer: PloytecEngine::UpdateTimestamp) */
static bool ReadAnchor(ozzy_shared_memory* shm, uint64_t* sampleTime, uint64_t* hostTime) {
    auto* ts = &shm->audio.timestamp;
    uint32_t seq = ozzy_load_acquire_u32(&ts->sequence);
    if (seq & 1) return false;
    *sampleTime = ts->sample_time;
    *hostTime = ts->host_time;
    return ozzy_load_acquire_u32(&ts->sequence) == seq;
}

int main(int argc, char** argv) {
    bool list = false, quiet = false;
    int index = 0, seconds = 0;
    const char* record = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) list = true;
        else if (!strcmp(argv[i], "-q")) quiet = true;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) record = argv[++i];
        else {
            fprintf(stderr, "usage: ozzy-harness [-l] [-d N] [-s seconds] [-r capture.wav] [-q]\n");
            return 2;
        }
    }
    fprintf(stderr, "ozzy-harness (build %s)\n", OZZY_GIT_VERSION);

    std::vector<FoundDevice> devices = FindDevices();
    for (size_t i = 0; list && i < devices.size(); i++)
        fprintf(stderr, "%zu: %s (%04x:%04x), driver %ls\n", i, devices[i].model->name, devices[i].model->vid,
                devices[i].model->pid, devices[i].service.empty() ? L"none" : devices[i].service.c_str());
    if (list) return 0;
    if (index < 0 || (size_t)index >= devices.size()) {
        fprintf(stderr, "no supported device found (-l lists them)\n");
        return 1;
    }
    const FoundDevice& found = devices[index];
    const struct ozzy_device* model = found.model;
    if (found.path.empty()) {
        fprintf(stderr, "%s uses driver '%ls', not WinUSB. Install WinUSB for the whole device with Zadig\n"
                        "(Options > List All Devices; pick the device, not an interface).\n",
                model->name, found.service.c_str());
        return 1;
    }
    if (model->family == OZZY_FAMILY_PLOYTEC_ISOC) {
        fprintf(stderr, "%s streams isochronous OUT, which WinUSB can't pace (no per-packet lengths)\n", model->name);
        return 1;
    }

    WinUsbBus bus;
    if (!bus.Open(found.path.c_str())) return 1;

    // The shared memory the kext would map for the HAL; here both sides are in this process
    auto* shm = (ozzy_shared_memory*)VirtualAlloc(nullptr, sizeof(ozzy_shared_memory), MEM_COMMIT | MEM_RESERVE,
                                                PAGE_READWRITE);
    if (!shm) return 1;
    shm->magic = OZZY_MAGIC;
    shm->version = OZZY_SHARED_VERSION;
    shm->vendor_id = bus.DeviceDescriptor().idVendor;
    shm->product_id = bus.DeviceDescriptor().idProduct;
    bus.GetString(bus.DeviceDescriptor().iManufacturer, shm->manufacturer_name, sizeof(shm->manufacturer_name));
    bus.GetString(bus.DeviceDescriptor().iProduct, shm->product_name, sizeof(shm->product_name));
    bus.GetString(bus.DeviceDescriptor().iSerialNumber, shm->serial_number, sizeof(shm->serial_number));
    fprintf(stderr, "device: %s %s (%s), %04x:%04x\n", shm->manufacturer_name, shm->product_name, model->name,
            shm->vendor_id, shm->product_id);

    struct ozzy_engine* engine = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
    if (!engine) return 1;
    bus.SetEngine(engine);
    if (!bus.Configure(engine)) return 1;
    ozzy_engine_attach(engine, bus.Bus(), shm);
    if (!ozzy_engine_start(engine)) {
        fprintf(stderr, "engine start failed\n");
        ozzy_engine_stop(engine);
        bus.Close();
        return 1;
    }

    uint32_t rate = shm->audio.sample_rate, in = shm->audio.input_channels, out = shm->audio.output_channels;
    fprintf(stderr, "running at %u Hz, %u in / %u out\n", rate, in, out);
    if (out && !quiet) fprintf(stderr, "tone: %.2f Hz on outputs 1/2\n", FillTone(shm, out, rate));

    Wav wav;
    ploytec_frame_fn decode = in ? ploytec_frame_decoder(in) : nullptr;
    if (record && decode && !wav.Open(record, rate, in)) fprintf(stderr, "can't write %s\n", record);

    SetConsoleCtrlHandler(OnCtrl, TRUE);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    uint64_t firstSample = 0, firstHost = 0, nextFrame = 0, lost = 0;
    bool haveFirst = false;
    DWORD start = GetTickCount(), lastStats = start;

    while (!gQuit.load() && (!seconds || GetTickCount() - start < (DWORD)seconds * 1000)) {
        ::Sleep(20);

        // MIDI in
        uint8_t midi[256];
        uint32_t n = ozzy_midi_ring_pop(&shm->midi_in, midi, sizeof(midi));
        if (n) {
            fprintf(stderr, "midi in:");
            for (uint32_t i = 0; i < n; i++) fprintf(stderr, " %02x", midi[i]);
            fprintf(stderr, "\n");
        }

        uint64_t sampleTime, hostTime;
        if (!ReadAnchor(shm, &sampleTime, &hostTime) || !sampleTime) continue;
        if (!haveFirst) {
            haveFirst = true;
            firstSample = sampleTime;
            firstHost = hostTime;
        }

        // Input up to 3 packets before the anchor (the IN transfers run alongside the OUT ones)
        if (wav.f) {
            uint64_t end = sampleTime > 3 * PLOYTEC_FRAMES_PER_PKT ? sampleTime - 3 * PLOYTEC_FRAMES_PER_PKT : 0;
            if (end > nextFrame + kRingFrames / 2) {
                lost += end - kRingFrames / 2 - nextFrame;
                nextFrame = end - kRingFrames / 2;
            }
            uint8_t s24[16 * 3];
            for (; nextFrame < end; nextFrame++) {
                uint32_t f = (uint32_t)(nextFrame % kRingFrames);
                decode(s24, shm->audio.input_buffer + (f / PLOYTEC_FRAMES_PER_PKT) * OZZY_MAX_PACKET_SIZE +
                                (f % PLOYTEC_FRAMES_PER_PKT) * PLOYTEC_IN_FRAME_SIZE);
                wav.Write(s24, 1);
            }
        }

        if (GetTickCount() - lastStats >= 1000) {
            lastStats = GetTickCount();
            WinUsbBus::Stats s = bus.TakeStats();
            double secs = (double)(hostTime - firstHost) / freq.QuadPart;
            fprintf(stderr, "clock %.1f Hz | out %u/s, in %u/s, errors %u%s\n",
                    secs > 0 ? (sampleTime - firstSample) / secs : 0.0, s.outDone, s.inDone, s.errors,
                    lost ? " | input frames lost" : "");
        }
    }

    fprintf(stderr, "stopping\n");
    ozzy_engine_stop(engine);
    bus.Close();
    wav.Close();
    free(engine);
    VirtualFree(shm, 0, MEM_RELEASE);
    return 0;
}
