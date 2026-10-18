#include "device.h"
#include "stream.h"

/* Component IDs of the two circuits; the names are the INF's AddInterface reference strings */
static const GUID render_component = { 0x87e5b7f1, 0xc03a, 0x4350, { 0xaf, 0x68, 0x72, 0x29, 0x07, 0x66, 0x7a, 0x9a } };
static const GUID capture_component = { 0x4b712dd5, 0x1ce4, 0x4d50, { 0xb5, 0xf0, 0x68, 0xa3, 0x27, 0x23, 0xa7, 0xae } };

/* A direction's format at @rate: the model's channels, packed 24-bit (S24_3LE, what the frame
 * codecs take). Stereo gets a speaker mask, more channels
 * none (direct out: a mixer's channels aren't surround speakers). */
static void fill_format(KSDATAFORMAT_WAVEFORMATEXTENSIBLE* f, uint32_t channels, uint32_t rate) {
    RtlZeroMemory(f, sizeof(*f));
    f->DataFormat.FormatSize = sizeof(*f);
    f->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    f->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    f->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

    WAVEFORMATEXTENSIBLE* w = &f->WaveFormatExt;
    w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels = (WORD)channels;
    w->Format.nSamplesPerSec = rate;
    w->Format.nBlockAlign = (WORD)(channels * 3);
    w->Format.nAvgBytesPerSec = rate * channels * 3;
    w->Format.wBitsPerSample = 24;
    w->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w->Samples.wValidBitsPerSample = 24;
    w->dwChannelMask = channels == 2 ? KSAUDIO_SPEAKER_STEREO : KSAUDIO_SPEAKER_DIRECTOUT;
    w->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
}

/* A circuit with a host pin (the stream, KSCATEGORY_AUDIO) and a bridge pin (the device's line
 * connector), and no elements: the mixer has no volume or mute to offer here */
NTSTATUS wcircuit_create(WDFDEVICE device, bool render, ACXCIRCUIT* out) {
    PAGED_CODE();
    struct wdev* dev = wdev_get(device);

    PACXCIRCUIT_INIT init = AcxCircuitInitAllocate(device);
    if (!init) return STATUS_INSUFFICIENT_RESOURCES;
    AcxCircuitInitSetComponentId(init, render ? &render_component : &capture_component);
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, render ? L"OzzyOut" : L"OzzyIn");
    (VOID)AcxCircuitInitAssignName(init, &name);
    AcxCircuitInitSetCircuitType(init, render ? AcxCircuitTypeRender : AcxCircuitTypeCapture);
    NTSTATUS status = AcxCircuitInitAssignAcxCreateStreamCallback(init, wstream_evt_create);
    if (NT_SUCCESS(status)) status = wsettings_assign(init);
    if (!NT_SUCCESS(status)) {
        AcxCircuitInitFree(init);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, wcircuit);
    ACXCIRCUIT circuit;
    status = AcxCircuitCreate(device, &attributes, &init, &circuit);
    if (!NT_SUCCESS(status)) {
        AcxCircuitInitFree(init);
        return status;
    }
    struct wcircuit* ctx = wcircuit_get(circuit);
    ctx->render = render;
    status = wsettings_add_event(circuit);
    if (!NT_SUCCESS(status)) return status;

    /* Pin 0: the host pin the stream connects to; pin 1: the bridge to the device */
    ACXPIN pins[2];
    ACX_PIN_CONFIG pin;
    ACX_PIN_CONFIG_INIT(&pin);
    pin.Type = render ? AcxPinTypeSink : AcxPinTypeSource;
    pin.Communication = AcxPinCommunicationSink;
    pin.Category = &KSCATEGORY_AUDIO;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = circuit;
    status = AcxPinCreate(circuit, &attributes, &pin, &pins[0]);
    if (!NT_SUCCESS(status)) return status;

    ACX_PIN_CONFIG_INIT(&pin);
    pin.Type = render ? AcxPinTypeSource : AcxPinTypeSink;
    pin.Communication = AcxPinCommunicationNone;
    pin.Category = &KSNODETYPE_LINE_CONNECTOR;
    status = AcxPinCreate(circuit, &attributes, &pin, &pins[1]);
    if (!NT_SUCCESS(status)) return status;

    /* A format per rate, the highest (the engine's default) first. A stream at another rate
     * restarts the engine at it (wdev_stream_rate). */
    ACXDATAFORMATLIST formats = AcxPinGetRawDataFormatList(pins[0]);
    if (!formats) return STATUS_INSUFFICIENT_RESOURCES;
    uint32_t rates[OZZY_MAX_RATES];
    unsigned int n = ozzy_device_rates(dev->model, rates);
    for (unsigned int i = 0; i < n; i++) {
        KSDATAFORMAT_WAVEFORMATEXTENSIBLE* f = &ctx->formats[i];
        fill_format(f, render ? dev->model->out_channels : dev->model->in_channels, rates[n - 1 - i]);
        ACX_DATAFORMAT_CONFIG format_config;
        ACX_DATAFORMAT_CONFIG_INIT_KS(&format_config, f);
        ACXDATAFORMAT format;
        status = AcxDataFormatCreate(device, &attributes, &format_config, &format);
        if (!NT_SUCCESS(status)) return status;
        status = AcxDataFormatListAddDataFormat(formats, format);
        if (!NT_SUCCESS(status)) return status;
    }

    status = AcxCircuitAddPins(circuit, pins, 2);
    if (!NT_SUCCESS(status)) return status;
    *out = circuit;
    return STATUS_SUCCESS;
}
