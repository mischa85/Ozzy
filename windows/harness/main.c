/* ozzy-harness: runs an Ozzy engine against a device bound to WinUSB, for protocol work on
 * Windows without loading a kernel driver. It plays the HAL's part of the shared memory: a
 * test tone in the output ring, the input ring to a WAV file, MIDI in to the console.
 *
 *   ozzy-harness [-l] [-d N] [-s seconds] [-r capture.wav] [-q]
 *     -l  list the supported devices found and what driver they use
 *     -d  the Nth of them (default 0)
 *     -s  stop after this many seconds (default: Ctrl+C)
 *     -r  record the input to a 24-bit WAV file
 *     -q  silence instead of the test tone
 */

#include "winusb_bus.h"
#include "../../engine/core/ozzy_engine.h"
#include "../../common/devices/ploytec/ploytec_protocol.h"
#include "../../engine/shared/ozzy_shared_data.h"
#include "../../common/devices/ploytec/ploytec_codec.h"
#include "../../common/ozzy_version.h"
#include <setupapi.h>
#include <objbase.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum { MAX_FOUND = 16 };

static volatile LONG quit;

static BOOL WINAPI on_ctrl(DWORD type) {
    (void)type;
    InterlockedExchange(&quit, 1);
    return TRUE;
}

struct found_device {
    const struct ozzy_device* model;
    wchar_t service[128];   /* the bound driver, e.g. WinUSB */
    wchar_t path[1024];     /* WinUSB device interface path, empty if there is none */
};

/* The device interface path of device @dev into @path, from the interface GUID its WinUSB INF
 * registered (Zadig writes DeviceInterfaceGUIDs; INFs written by hand often DeviceInterfaceGUID) */
static void winusb_path(HDEVINFO set, SP_DEVINFO_DATA* dev, wchar_t* path, size_t path_chars) {
    path[0] = 0;
    wchar_t guid_text[128] = { 0 };
    HKEY key = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) return;
    DWORD size = sizeof(guid_text) - sizeof(wchar_t);
    bool ok = RegQueryValueExW(key, L"DeviceInterfaceGUIDs", NULL, NULL, (BYTE*)guid_text, &size) == ERROR_SUCCESS;
    size = sizeof(guid_text) - sizeof(wchar_t);
    if (!ok) ok = RegQueryValueExW(key, L"DeviceInterfaceGUID", NULL, NULL, (BYTE*)guid_text, &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    GUID guid;
    if (!ok || CLSIDFromString(guid_text, &guid) != NOERROR) return;

    /* With DIGCF_DEVICEINTERFACE the enumerator can be a device instance ID: only this device */
    wchar_t instance[512];
    if (!SetupDiGetDeviceInstanceIdW(set, dev, instance, 512, NULL)) return;
    HDEVINFO ifs = SetupDiGetClassDevsW(&guid, instance, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (ifs == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA data = { sizeof(data) };
    if (SetupDiEnumDeviceInterfaces(ifs, NULL, &guid, 0, &data)) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(ifs, &data, NULL, 0, &needed, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* detail = needed ? malloc(needed) : NULL;
        if (detail) {
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(ifs, &data, detail, needed, NULL, NULL))
                wcsncpy_s(path, path_chars, detail->DevicePath, _TRUNCATE);
            free(detail);
        }
    }
    SetupDiDestroyDeviceInfoList(ifs);
}

/* Present USB devices that are in common/ozzy_devices.def (whole devices, not interfaces), at
 * most @max into @found; returns how many */
static int find_devices(struct found_device* found, int max) {
    int count = 0;
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return 0;
    SP_DEVINFO_DATA dev = { sizeof(dev) };
    for (DWORD i = 0; count < max && SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        wchar_t ids[1024] = { 0 };
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, NULL, (BYTE*)ids,
                                               sizeof(ids) - 2 * sizeof(wchar_t), NULL))
            continue;
        unsigned int vid, pid;
        if (swscanf_s(ids, L"USB\\VID_%4x&PID_%4x", &vid, &pid) != 2 || wcsstr(ids, L"&MI_")) continue;
        const struct ozzy_device* model = ozzy_device_lookup((uint16_t)vid, (uint16_t)pid);
        if (!model) continue;

        struct found_device* f = &found[count++];
        memset(f, 0, sizeof(*f));
        f->model = model;
        SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_SERVICE, NULL, (BYTE*)f->service,
                                          sizeof(f->service) - sizeof(wchar_t), NULL);
        if (_wcsicmp(f->service, L"WinUSB") == 0) winusb_path(set, &dev, f->path, sizeof(f->path) / sizeof(wchar_t));
    }
    SetupDiDestroyDeviceInfoList(set);
    return count;
}

/* 24-bit WAV writer (sizes patched on close) */
struct wav {
    FILE* f;
    uint32_t channels, bytes;
};

static bool wav_open(struct wav* w, const char* name, uint32_t rate, uint32_t channels) {
    if (fopen_s(&w->f, name, "wb") != 0) return false;
    w->channels = channels;
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0 };
    uint32_t block_align = channels * 3, byte_rate = rate * block_align;
    h[22] = (uint8_t)channels;
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    h[32] = (uint8_t)block_align;
    h[34] = 24;
    memcpy(h + 36, "data", 4);
    fwrite(h, 1, sizeof(h), w->f);
    return true;
}

static void wav_write(struct wav* w, const uint8_t* s24, uint32_t frames) {
    w->bytes += (uint32_t)fwrite(s24, 1, frames * w->channels * 3, w->f);
}

static void wav_close(struct wav* w) {
    if (!w->f) return;
    uint32_t riff = 36 + w->bytes;
    fseek(w->f, 4, SEEK_SET);
    fwrite(&riff, 4, 1, w->f);
    fseek(w->f, 40, SEEK_SET);
    fwrite(&w->bytes, 4, 1, w->f);
    fclose(w->f);
    w->f = NULL;
}

enum { RING_FRAMES = OZZY_NUM_PACKETS * PLOYTEC_FRAMES_PER_PKT };

/* A -18 dBFS sine on outputs 1 and 2 with a whole number of cycles in the output ring, so the
 * engine can loop the ring forever without a position from us. Written with the float frame
 * codec into the frames of each logical packet; the MIDI and sync bytes stay as the engine's
 * start set them. */
static double fill_tone(struct ozzy_shared_memory* shm, uint32_t channels, uint32_t rate) {
    ploytec_frame_f32_enc_fn encode = ploytec_frame_encoder_f32(channels);
    if (!encode) return 0;
    enum ploytec_out_layout layout = (shm->audio.device_flags & OZZY_DEVICE_FLAG_PLOYTEC_BULK) ? PLOYTEC_OUT_LAYOUT_BULK
                                                                                            : PLOYTEC_OUT_LAYOUT_INT;
    const uint32_t cycles = 48;
    float frame[16] = { 0 };
    for (uint32_t f = 0; f < RING_FRAMES; f++) {
        float v = 0.125f * (float)sin(2.0 * 3.14159265358979 * cycles * f / RING_FRAMES);
        frame[0] = frame[1] = v;
        encode(shm->audio.output_buffer + (f / PLOYTEC_FRAMES_PER_PKT) * OZZY_MAX_PACKET_SIZE +
               ploytec_out_frame_offset(layout, f % PLOYTEC_FRAMES_PER_PKT), frame);
    }
    return (double)cycles * rate / RING_FRAMES;
}

/* The latest timestamp anchor (seqlock, writer: the engine's timestamp update) */
static bool read_anchor(struct ozzy_shared_memory* shm, uint64_t* sample_time, uint64_t* host_time) {
    volatile struct ozzy_timestamp* ts = &shm->audio.timestamp;
    uint32_t seq = ozzy_load_acquire_u32(&ts->sequence);
    if (seq & 1) return false;
    *sample_time = ts->sample_time;
    *host_time = ts->host_time;
    return ozzy_load_acquire_u32(&ts->sequence) == seq;
}

int main(int argc, char** argv) {
    bool list = false, quiet = false;
    int index = 0, seconds = 0;
    const char* record = NULL;
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

    static struct found_device devices[MAX_FOUND];
    int device_count = find_devices(devices, MAX_FOUND);
    for (int i = 0; list && i < device_count; i++)
        fprintf(stderr, "%d: %s (%04x:%04x), driver %ls\n", i, devices[i].model->name, devices[i].model->vid,
                devices[i].model->pid, devices[i].service[0] ? devices[i].service : L"none");
    if (list) return 0;
    if (index < 0 || index >= device_count) {
        fprintf(stderr, "no supported device found (-l lists them)\n");
        return 1;
    }
    const struct found_device* found = &devices[index];
    const struct ozzy_device* model = found->model;
    if (!found->path[0]) {
        fprintf(stderr, "%s uses driver '%ls', not WinUSB. Install WinUSB for the whole device with Zadig\n"
                        "(Options > List All Devices; pick the device, not an interface).\n",
                model->name, found->service);
        return 1;
    }
    if (model->family == OZZY_FAMILY_PLOYTEC_ISOC) {
        fprintf(stderr, "%s streams isochronous OUT, which WinUSB can't pace (no per-packet lengths)\n", model->name);
        return 1;
    }

    size_t engine_size = ozzy_engine_size(model);
    struct ozzy_engine* engine = ozzy_engine_init(engine_size ? calloc(1, engine_size) : NULL, model);
    if (!engine) return 1;

    static struct winusb_bus bus;
    if (!wbus_open(&bus, found->path, engine)) {
        wbus_close(&bus);
        return 1;
    }

    /* The shared memory the kext would map for the HAL; here both sides are in this process */
    struct ozzy_shared_memory* shm = VirtualAlloc(NULL, sizeof(*shm), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!shm) return 1;
    wbus_get_string(&bus, bus.device_desc.iManufacturer, shm->manufacturer_name, sizeof(shm->manufacturer_name));
    wbus_get_string(&bus, bus.device_desc.iProduct, shm->product_name, sizeof(shm->product_name));
    wbus_get_string(&bus, bus.device_desc.iSerialNumber, shm->serial_number, sizeof(shm->serial_number));
    fprintf(stderr, "device: %s %s (%s), %04x:%04x\n", shm->manufacturer_name, shm->product_name, model->name,
            model->vid, model->pid);

    if (!wbus_configure(&bus)) return 1;
    ozzy_engine_attach(engine, &bus.bus, shm);
    if (!ozzy_engine_start(engine)) {
        fprintf(stderr, "engine start failed\n");
        ozzy_engine_stop(engine);
        wbus_close(&bus);
        return 1;
    }

    uint32_t rate = shm->audio.sample_rate, in = shm->audio.input_channels, out = shm->audio.output_channels;
    fprintf(stderr, "running at %u Hz, %u in / %u out\n", rate, in, out);
    if (out && !quiet) fprintf(stderr, "tone: %.2f Hz on outputs 1/2\n", fill_tone(shm, out, rate));

    struct wav wav = { 0 };
    ploytec_frame_fn decode = in ? ploytec_frame_decoder(in) : NULL;
    if (record && decode && !wav_open(&wav, record, rate, in)) fprintf(stderr, "can't write %s\n", record);

    SetConsoleCtrlHandler(on_ctrl, TRUE);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    uint64_t first_sample = 0, first_host = 0, next_frame = 0, lost = 0;
    bool have_first = false;
    DWORD start = GetTickCount(), last_stats = start;

    while (!InterlockedCompareExchange(&quit, 0, 0) && (!seconds || GetTickCount() - start < (DWORD)seconds * 1000)) {
        Sleep(20);

        /* MIDI in */
        uint8_t midi[256];
        uint32_t n = ozzy_midi_ring_pop(&shm->midi_in, midi, sizeof(midi));
        if (n) {
            fprintf(stderr, "midi in:");
            for (uint32_t i = 0; i < n; i++) fprintf(stderr, " %02x", midi[i]);
            fprintf(stderr, "\n");
        }

        uint64_t sample_time, host_time;
        if (!read_anchor(shm, &sample_time, &host_time) || !sample_time) continue;
        if (!have_first) {
            have_first = true;
            first_sample = sample_time;
            first_host = host_time;
        }

        /* Input up to 3 packets before the anchor (the IN transfers run alongside the OUT ones) */
        if (wav.f) {
            uint64_t end = sample_time > 3 * PLOYTEC_FRAMES_PER_PKT ? sample_time - 3 * PLOYTEC_FRAMES_PER_PKT : 0;
            if (end > next_frame + RING_FRAMES / 2) {
                lost += end - RING_FRAMES / 2 - next_frame;
                next_frame = end - RING_FRAMES / 2;
            }
            uint8_t s24[16 * 3];
            for (; next_frame < end; next_frame++) {
                uint32_t f = (uint32_t)(next_frame % RING_FRAMES);
                decode(s24, shm->audio.input_buffer + (f / PLOYTEC_FRAMES_PER_PKT) * OZZY_MAX_PACKET_SIZE +
                                (f % PLOYTEC_FRAMES_PER_PKT) * PLOYTEC_IN_FRAME_SIZE);
                wav_write(&wav, s24, 1);
            }
        }

        if (GetTickCount() - last_stats >= 1000) {
            last_stats = GetTickCount();
            struct wbus_stats s = wbus_take_stats(&bus);
            double secs = (double)(host_time - first_host) / freq.QuadPart;
            fprintf(stderr, "clock %.1f Hz | out %u/s, in %u/s, errors %u%s\n",
                    secs > 0 ? (sample_time - first_sample) / secs : 0.0, s.out_done, s.in_done, s.errors,
                    lost ? " | input frames lost" : "");
        }
    }

    fprintf(stderr, "stopping\n");
    ozzy_engine_stop(engine);
    wbus_close(&bus);
    wav_close(&wav);
    free(engine);
    VirtualFree(shm, 0, MEM_RELEASE);
    return 0;
}
