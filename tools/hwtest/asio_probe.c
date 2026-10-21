// SPDX-License-Identifier: MIT
/*
 * asio_probe: talk to an installed ASIO driver (Windows) without a DAW, to see what another
 * driver does on the wire while it is captured: its buffer sizes and rates, a rate change, and
 * its control panel (the "ASIO settings" button a DAW has).
 *
 *   asio_probe list                     the ASIO drivers registered
 *   asio_probe info  [NAME-PART]        channels, buffer sizes, latencies, sample rate
 *   asio_probe rate  RATE [NAME-PART]   set the sample rate (the driver switches the device)
 *   asio_probe panel [NAME-PART]        open the driver's control panel; run it on the desktop,
 *                                       not over SSH; returns when its windows are closed
 *
 * ASIO drivers are COM objects whose class ID is also their interface ID; the interface layout
 * below is the one every ASIO driver implements (no SDK needed for these calls).
 *
 * Build (VS developer prompt): cl /O2 asio_probe.c ole32.lib advapi32.lib user32.lib
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct iasio iasio;

/* The ASIO driver interface: IUnknown, then the ASIO calls in their fixed order */
struct iasio_vtbl {
	HRESULT (STDMETHODCALLTYPE *query_interface)(iasio *, REFIID, void **);
	ULONG (STDMETHODCALLTYPE *add_ref)(iasio *);
	ULONG (STDMETHODCALLTYPE *release)(iasio *);
	long (STDMETHODCALLTYPE *init)(iasio *, void *sys_handle);
	void (STDMETHODCALLTYPE *get_driver_name)(iasio *, char *name);
	long (STDMETHODCALLTYPE *get_driver_version)(iasio *);
	void (STDMETHODCALLTYPE *get_error_message)(iasio *, char *msg);
	long (STDMETHODCALLTYPE *start)(iasio *);
	long (STDMETHODCALLTYPE *stop)(iasio *);
	long (STDMETHODCALLTYPE *get_channels)(iasio *, long *in, long *out);
	long (STDMETHODCALLTYPE *get_latencies)(iasio *, long *in, long *out);
	long (STDMETHODCALLTYPE *get_buffer_size)(iasio *, long *min, long *max, long *preferred, long *granularity);
	long (STDMETHODCALLTYPE *can_sample_rate)(iasio *, double rate);
	long (STDMETHODCALLTYPE *get_sample_rate)(iasio *, double *rate);
	long (STDMETHODCALLTYPE *set_sample_rate)(iasio *, double rate);
	void *get_clock_sources, *set_clock_source, *get_sample_position, *get_channel_info;
	void *create_buffers, *dispose_buffers;
	long (STDMETHODCALLTYPE *control_panel)(iasio *);
	void *future, *output_ready;
};

struct iasio {
	const struct iasio_vtbl *v;
};

#define ASE_OK 0

/* The first registered driver whose key name contains @part: its name and class ID */
static int find_driver(const char *part, char *name, size_t name_size, CLSID *clsid, int list)
{
	HKEY asio;
	DWORD i;
	int found = 0;

	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &asio) != ERROR_SUCCESS)
		return 0;
	for (i = 0; !found; i++) {
		char key[256], id[64];
		DWORD key_len = sizeof(key), id_len = sizeof(id);
		WCHAR wid[64];

		if (RegEnumKeyExA(asio, i, key, &key_len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
			break;
		if (RegGetValueA(asio, key, "CLSID", RRF_RT_REG_SZ, NULL, id, &id_len) != ERROR_SUCCESS)
			continue;
		if (list) {
			printf("%s  %s\n", key, id);
			continue;
		}
		if (part && !strstr(key, part))
			continue;
		MultiByteToWideChar(CP_ACP, 0, id, -1, wid, 64);
		if (CLSIDFromString(wid, clsid) == NOERROR) {
			snprintf(name, name_size, "%s", key);
			found = 1;
		}
	}
	RegCloseKey(asio);
	return found;
}

static void info(iasio *d)
{
	long in = 0, out = 0, min = 0, max = 0, pref = 0, gran = 0, lin = 0, lout = 0;
	double rate = 0;
	static const double rates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
	char name[64] = "";
	int i;

	d->v->get_driver_name(d, name);
	d->v->get_channels(d, &in, &out);
	d->v->get_buffer_size(d, &min, &max, &pref, &gran);
	d->v->get_latencies(d, &lin, &lout);
	d->v->get_sample_rate(d, &rate);
	printf("driver %s, version %ld\n", name, d->v->get_driver_version(d));
	printf("channels %ld in / %ld out\n", in, out);
	printf("buffer min %ld, max %ld, preferred %ld, granularity %ld\n", min, max, pref, gran);
	printf("latency in %ld, out %ld frames\n", lin, lout);
	printf("sample rate %.0f; can:", rate);
	for (i = 0; i < (int)(sizeof(rates) / sizeof(rates[0])); i++)
		if (d->v->can_sample_rate(d, rates[i]) == ASE_OK)
			printf(" %.0f", rates[i]);
	printf("\n");
}

/* Pump messages until this thread has no visible top-level window left (the panel closed) */
static BOOL CALLBACK any_visible(HWND w, LPARAM found)
{
	if (IsWindowVisible(w)) {
		*(int *)found = 1;
		return FALSE;
	}
	return TRUE;
}

static void run_panel(iasio *d)
{
	int seen = 0;
	DWORD idle_since = GetTickCount();

	printf("control panel: %ld\n", d->v->control_panel(d));
	for (;;) {
		MSG m;
		int found = 0;

		while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&m);
			DispatchMessageA(&m);
		}
		EnumThreadWindows(GetCurrentThreadId(), any_visible, (LPARAM)&found);
		if (found) {
			seen = 1;
			idle_since = GetTickCount();
		} else if (GetTickCount() - idle_since > (seen ? 500u : 5000u)) {
			break;   /* closed, or a panel that runs in another process */
		}
		Sleep(20);
	}
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "list";
	const char *part = NULL;
	char name[256];
	CLSID clsid;
	iasio *d = NULL;
	HWND owner;
	int ret = 0;

	if (!strcmp(cmd, "rate"))
		part = argc > 3 ? argv[3] : NULL;
	else
		part = argc > 2 ? argv[2] : NULL;
	CoInitialize(NULL);
	if (!strcmp(cmd, "list")) {
		find_driver(NULL, name, sizeof(name), &clsid, 1);
		return 0;
	}
	if (!find_driver(part, name, sizeof(name), &clsid, 0)) {
		fprintf(stderr, "asio_probe: no ASIO driver named like \"%s\"\n", part ? part : "");
		return 1;
	}
	if (CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &clsid, (void **)&d) != S_OK || !d) {
		fprintf(stderr, "asio_probe: can't load %s (a 32-bit driver needs a 32-bit build)\n", name);
		return 1;
	}
	/* Drivers want a window for init (the panel's owner, messages) */
	owner = CreateWindowA("STATIC", "asio_probe", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
	if (!d->v->init(d, owner)) {
		char msg[128] = "";

		d->v->get_error_message(d, msg);
		fprintf(stderr, "asio_probe: %s: init failed: %s\n", name, msg);
		d->v->release(d);
		return 1;
	}
	if (!strcmp(cmd, "info")) {
		info(d);
	} else if (!strcmp(cmd, "rate") && argc > 2) {
		double want = atof(argv[2]), now = 0;
		long st = d->v->set_sample_rate(d, want);

		Sleep(500);
		d->v->get_sample_rate(d, &now);
		printf("set %.0f: %ld, runs at %.0f\n", want, st, now);
		ret = st == ASE_OK && now == want ? 0 : 1;
	} else if (!strcmp(cmd, "panel")) {
		run_panel(d);
		info(d);
	} else {
		fprintf(stderr, "usage: asio_probe list | info [NAME] | rate RATE [NAME] | panel [NAME]\n");
		ret = 2;
	}
	d->v->release(d);
	DestroyWindow(owner);
	CoUninitialize();
	return ret;
}
