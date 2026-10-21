// SPDX-License-Identifier: MIT
/*
 * wasapi_probe: which formats an endpoint takes in WASAPI exclusive mode, by channel mask and rate
 * (IAudioClient::IsFormatSupported), plus its shared-mode mix format and periods. For drivers whose
 * format list a client library won't open: it shows whether the format, the mask or the rate is
 * refused.
 *
 *   wasapi_probe Xone            every render and capture endpoint whose name contains "Xone"
 *
 * Build (VS developer prompt): cl /O2 wasapi_probe.c ole32.lib
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <stdio.h>

/* The headers only declare these in C: their fixed values */
static const GUID clsid_enumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID iid_enumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID iid_audio_client = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID subtype_pcm = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

static void fill(WAVEFORMATEXTENSIBLE *w, WORD channels, DWORD rate, WORD container, WORD valid, DWORD mask)
{
	ZeroMemory(w, sizeof(*w));
	w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	w->Format.nChannels = channels;
	w->Format.nSamplesPerSec = rate;
	w->Format.wBitsPerSample = container;
	w->Format.nBlockAlign = (WORD)(channels * container / 8);
	w->Format.nAvgBytesPerSec = rate * w->Format.nBlockAlign;
	w->Format.cbSize = sizeof(*w) - sizeof(WAVEFORMATEX);
	w->Samples.wValidBitsPerSample = valid;
	w->dwChannelMask = mask;
	w->SubFormat = subtype_pcm;
}

static void probe(IMMDevice *dev, const wchar_t *name)
{
	static const DWORD rates[] = { 44100, 48000, 88200, 96000 };
	static const struct { DWORD mask; const char *what; } masks[] = {
		{ 0, "direct out (0)" }, { KSAUDIO_SPEAKER_7POINT1_SURROUND, "7.1 surround" }, { KSAUDIO_SPEAKER_7POINT1, "7.1 (wide)" },
	};
	IAudioClient *c = NULL;
	WAVEFORMATEX *mix = NULL;
	REFERENCE_TIME def = 0, min = 0;
	int m, r;

	if (FAILED(IMMDevice_Activate(dev, &iid_audio_client, CLSCTX_ALL, NULL, (void **)&c))) {
		wprintf(L"%ls: can't activate\n", name);
		return;
	}
	IAudioClient_GetMixFormat(c, &mix);
	IAudioClient_GetDevicePeriod(c, &def, &min);
	wprintf(L"%ls\n  mix format: %u ch, %lu Hz, %u bit; period default %.2f ms, minimum %.2f ms\n", name,
		mix->nChannels, mix->nSamplesPerSec, mix->wBitsPerSample, def / 10000.0, min / 10000.0);
	if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
		wprintf(L"  mix channel mask 0x%lx\n", ((WAVEFORMATEXTENSIBLE *)mix)->dwChannelMask);
	for (m = 0; m < 3; m++) {
		printf("  exclusive, 24-in-24, %-15s:", masks[m].what);
		for (r = 0; r < 4; r++) {
			WAVEFORMATEXTENSIBLE w;
			HRESULT hr;

			fill(&w, mix->nChannels, rates[r], 24, 24, masks[m].mask);
			hr = IAudioClient_IsFormatSupported(c, AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX *)&w, NULL);
			printf(" %lu %s", rates[r], hr == S_OK ? "ok" : "no");
		}
		printf("\n");
	}
	CoTaskMemFree(mix);
	IAudioClient_Release(c);
}

int wmain(int argc, wchar_t **argv)
{
	const wchar_t *part = argc > 1 ? argv[1] : L"";
	IMMDeviceEnumerator *e = NULL;
	EDataFlow flows[] = { eRender, eCapture };
	int f;

	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(CoCreateInstance(&clsid_enumerator, NULL, CLSCTX_ALL, &iid_enumerator, (void **)&e)))
		return 1;
	for (f = 0; f < 2; f++) {
		IMMDeviceCollection *list = NULL;
		UINT n = 0, i;

		IMMDeviceEnumerator_EnumAudioEndpoints(e, flows[f], DEVICE_STATE_ACTIVE, &list);
		IMMDeviceCollection_GetCount(list, &n);
		for (i = 0; i < n; i++) {
			IMMDevice *dev = NULL;
			IPropertyStore *props = NULL;
			PROPVARIANT v;

			IMMDeviceCollection_Item(list, i, &dev);
			IMMDevice_OpenPropertyStore(dev, STGM_READ, &props);
			PropVariantInit(&v);
			IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &v);
			if (v.pwszVal && wcsstr(v.pwszVal, part)) {
				wprintf(L"%ls ", flows[f] == eRender ? L"render:" : L"capture:");
				probe(dev, v.pwszVal);
			}
			PropVariantClear(&v);
			IPropertyStore_Release(props);
			IMMDevice_Release(dev);
		}
		IMMDeviceCollection_Release(list);
	}
	IMMDeviceEnumerator_Release(e);
	CoUninitialize();
	return 0;
}
