/* SPDX-License-Identifier: MIT */
/*
 * struct ozzy_info (engine/shared/ozzy_info.h), what ozzy-cli and the ozzy app show: a start
 * publishes it with the model, the pipes, the stream format and what the handshake read; a
 * restart at another rate updates it and counts the start; the USB speed the platform sets
 * shows.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

int main(void)
{
	const struct ozzy_device *model = ozzy_device_lookup(0x0A4A, 0xFFDB);   /* Xone:DB4 */
	static struct ozzy_shared_memory shm;
	const struct ozzy_info *info = &shm.info;
	struct fake_bus fb;
	struct ozzy_engine *e;

	fake_bus_init(&fb, &shm, true, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x05, 3, 964, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 3, 1024, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_IN, 0x83, 3, 64, 1);
	ozzy_engine_attach(e, &fb.bus, &shm);
	ozzy_engine_set_usb_speed(e, OZZY_USB_SPEED_HIGH);
	ozzy_engine_set_driver_version(e, "test-1234");
	CHECK(info->version == 0, "info before the first start");
	CHECK(ozzy_engine_start(e), "start");

	CHECK(info->version == OZZY_INFO_VERSION && info->size == sizeof(*info), "version %u size %u",
	      info->version, info->size);
	CHECK(info->starts == 1, "starts %u", info->starts);
	CHECK(info->vendor_id == 0x0A4A && info->product_id == 0xFFDB, "USB ID %04x:%04x", info->vendor_id,
	      info->product_id);
	CHECK(info->family == OZZY_FAMILY_PLOYTEC_XONE && info->status == model->status, "family %u status %u",
	      info->family, info->status);
	CHECK(info->usb_speed == OZZY_USB_SPEED_HIGH, "USB speed %u", info->usb_speed);
	CHECK(!strcmp(info->driver_version, "test-1234"), "driver version \"%s\"", info->driver_version);
	CHECK(info->output == OZZY_INFO_OUTPUT_PLOYTEC_INTERRUPT, "output %u (964-byte interrupt EP)", info->output);
	CHECK(info->feedback == OZZY_INFO_FEEDBACK_NONE, "feedback %u", info->feedback);
	CHECK(info->firmware_valid && info->firmware[0] == 1, "firmware %u.%u.%u", info->firmware[0],
	      info->firmware[1], info->firmware[2]);
	CHECK(info->input_selector_valid, "input selector not read");
	CHECK(info->sample_rate == shm.audio.sample_rate && info->sample_rate == 96000, "rate %u", info->sample_rate);
	CHECK(info->device_rates == model->rates, "device rates %#x", info->device_rates);
	CHECK(info->host_rates == model->rates, "host rates %#x (not locked: all the device lists)", info->host_rates);
	CHECK(info->in_channels == 8 && info->out_channels == 8, "channels %u / %u", info->in_channels,
	      info->out_channels);
	CHECK(info->transfer_frames[OZZY_PCM_OUT] && info->transfer_frames[OZZY_PCM_IN], "transfer frames %u / %u",
	      info->transfer_frames[OZZY_PCM_OUT], info->transfer_frames[OZZY_PCM_IN]);
	CHECK(info->out_bytes_per_frame == shm.audio.output_bytes_per_frame &&
	      info->in_bytes_per_frame == shm.audio.input_bytes_per_frame, "bytes per frame");
	CHECK(info->safety_offset_frames == shm.audio.safety_offset_frames && info->safety_offset_frames,
	      "safety offset %u", info->safety_offset_frames);
	CHECK(info->timestamp_frames == OZZY_TIMESTAMP_FRAMES, "timestamp frames %u", info->timestamp_frames);
	CHECK(info->input_safety_offset_frames == 4 * 80, "input safety offset %u", info->input_safety_offset_frames);
	CHECK(info->pipes[OZZY_PIPE_PCM_OUT].address == 0x05 && info->pipes[OZZY_PIPE_PCM_OUT].transfer_type == 3 &&
	      info->pipes[OZZY_PIPE_PCM_OUT].max_packet == 964 && info->pipes[OZZY_PIPE_PCM_OUT].in_flight == 4,
	      "PCM out pipe %#x type %u %u bytes, %u in flight", info->pipes[OZZY_PIPE_PCM_OUT].address,
	      info->pipes[OZZY_PIPE_PCM_OUT].transfer_type, info->pipes[OZZY_PIPE_PCM_OUT].max_packet,
	      info->pipes[OZZY_PIPE_PCM_OUT].in_flight);
	CHECK(info->pipes[OZZY_PIPE_MIDI_IN].address == 0x83 && info->pipes[OZZY_PIPE_MIDI_IN].in_flight == 4,
	      "MIDI in pipe");
	CHECK(info->pipes[OZZY_PIPE_FEEDBACK].address == 0 && info->pipes[OZZY_PIPE_FEEDBACK].in_flight == 0,
	      "a feedback pipe on a Xone");

	/* Another rate: the restart publishes it, and counts */
	CHECK(ozzy_engine_change_rate(e, 44100) == OZZY_SETTING_RESTART && ozzy_engine_restart(e), "rate restart");
	CHECK(info->sample_rate == 44100 && info->starts == 2, "after the restart: %u Hz, %u starts", info->sample_rate,
	      info->starts);

	ozzy_engine_stop(e);
	free(e);
	return CHECK_DONE("info");
}
