// SPDX-License-Identifier: MIT
/*
 * rtlat: round-trip latency through an audio interface that loops its playback back to its
 * capture (the Xones loop each USB return to their USB send). A one-sample click every half
 * second on OUT, found again on IN, counted in JACK frames, so sample-exact. Runs on JACK, or on
 * PipeWire through its JACK layer:
 *
 *   pw-jack ./rtlat "Allen&Heath Xone DB4 Pro:playback_AUX4" "Allen&Heath Xone DB4 Pro:capture_AUX4" 6
 *
 * Pick a channel nothing else plays on. The result includes the host's buffering (two periods at
 * least), the driver's and the device's; "reported" is what the ports claim.
 *
 * Build: cc -O2 -o rtlat rtlat.c -ljack -lm
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <jack/jack.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_CLICKS 64

static jack_port_t *port_in, *port_out;
static volatile unsigned long long now;   /* frames processed */
static unsigned long long sent_at, spacing;
static volatile int found;
static long round_trip[MAX_CLICKS];
static int waiting;

static int process(jack_nframes_t n, void *arg)
{
	float *out = jack_port_get_buffer(port_out, n), *in = jack_port_get_buffer(port_in, n);
	jack_nframes_t k;

	(void)arg;
	for (k = 0; k < n; k++, now++) {
		out[k] = 0;
		if (now % spacing == 0 && now >= spacing) {
			out[k] = 0.5f;
			sent_at = now;
			waiting = 1;
		}
		if (waiting && fabsf(in[k]) > 0.05f) {
			if (found < MAX_CLICKS)
				round_trip[found++] = (long)(now - sent_at);
			waiting = 0;
		}
		if (waiting && now - sent_at > spacing - 10)
			waiting = 0;   /* lost: never came back */
	}
	return 0;
}

int main(int argc, char **argv)
{
	int want = argc > 3 ? atoi(argv[3]) : 10, i;
	jack_latency_range_t lat_out, lat_in;
	jack_client_t *c;

	if (argc < 3) {
		fprintf(stderr, "usage: rtlat OUT-PORT IN-PORT [CLICKS]\n");
		return 2;
	}
	c = jack_client_open("rtlat", JackNoStartServer, NULL);
	if (!c) {
		fprintf(stderr, "rtlat: no JACK server (on PipeWire: pw-jack ./rtlat ...)\n");
		return 1;
	}
	spacing = jack_get_sample_rate(c) / 2;
	port_out = jack_port_register(c, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
	port_in = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	jack_set_process_callback(c, process, NULL);
	jack_activate(c);
	if (jack_connect(c, "rtlat:out", argv[1]) || jack_connect(c, argv[2], "rtlat:in")) {
		fprintf(stderr, "rtlat: can't connect %s / %s\n", argv[1], argv[2]);
		return 1;
	}
	while (found < want && now < spacing * (want + 6))
		usleep(50000);
	jack_port_get_latency_range(jack_port_by_name(c, argv[1]), JackPlaybackLatency, &lat_out);
	jack_port_get_latency_range(jack_port_by_name(c, argv[2]), JackCaptureLatency, &lat_in);
	printf("rate %u, buffer %u, reported out %u-%u in %u-%u\n", jack_get_sample_rate(c), jack_get_buffer_size(c),
	       lat_out.min, lat_out.max, lat_in.min, lat_in.max);
	printf("round trips (frames):");
	for (i = 0; i < found; i++)
		printf(" %ld", round_trip[i]);
	printf("%s\n", found ? "" : " none (is the loop on that channel?)");
	jack_client_close(c);
	return found ? 0 : 1;
}
