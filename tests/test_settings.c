/* SPDX-License-Identifier: MIT */
/*
 * ozzy_engine_change_setting, ozzy_engine_change_rate and ozzy_engine_restart, as every platform
 * now uses them: a device setting asks for a restart, a route applies at once, bad ids and
 * values are refused; a rate the device lists asks for a restart, the S/PDIF lock leaves only
 * the lock rate; a restart aborts what is in flight (the bus's abort) and runs the handshake
 * again, and stop leaves nothing in flight.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

/* Transfers queued on @fb */
static unsigned int queued(const struct fake_bus *fb)
{
	return fb->tail - fb->head;
}

int main(void)
{
	/* A DB4 with a digital input (clock source) and output routing, which no Xone has yet */
	struct ozzy_device model = *ozzy_device_lookup(0x0A4A, 0xFFDB);
	static struct ozzy_shared_memory shm;
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;

	model.quirks |= OZZY_QUIRK_DIGITAL_IN | OZZY_QUIRK_OUT_ROUTING;
	fake_bus_init(&fb, &shm, true, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(&model)), &model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x05, 3, 964, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 3, 1024, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_IN, 0x83, 3, 64, 1);
	ozzy_engine_attach(e, &fb.bus, &shm);
	CHECK(ozzy_engine_start(e), "start");
	CHECK(fake_bus_pop(&fb, &x), "nothing in flight after start");
	fake_bus_complete(&fb, e, &x, NULL, NULL);

	CHECK(ozzy_engine_setting_values(e, OZZY_SETTING_CLOCK_SOURCE) == OZZY_NUM_CLOCK_SOURCES,
	      "clock source values %u", ozzy_engine_setting_values(e, OZZY_SETTING_CLOCK_SOURCE));
	CHECK(ozzy_engine_setting_values(e, OZZY_SETTING_SPDIF_MIRROR) == 0, "a setting the device lacks has values");
	CHECK(ozzy_engine_change_setting(e, OZZY_SETTING_SPDIF_MIRROR, 0) == OZZY_SETTING_INVALID, "missing setting taken");
	CHECK(ozzy_engine_change_setting(e, OZZY_SETTING_CLOCK_SOURCE, OZZY_NUM_CLOCK_SOURCES) == OZZY_SETTING_INVALID,
	      "out of range value taken");
	CHECK(ozzy_engine_change_setting(e, OZZY_NUM_SETTINGS, 0) == OZZY_SETTING_INVALID, "bad id taken");
	CHECK(ozzy_engine_change_setting(e, OZZY_SETTING_CLOCK_SOURCE, OZZY_CLOCK_AUTOMATIC) == OZZY_SETTING_SAME,
	      "the default value again is a change");
	CHECK(ozzy_engine_change_setting(e, OZZY_SETTING_CLOCK_SOURCE, OZZY_CLOCK_INTERNAL) == OZZY_SETTING_RESTART,
	      "a device setting doesn't ask for a restart");
	CHECK(ozzy_engine_change_setting(e, OZZY_SETTING_OUT_ROUTE_1, 1) == OZZY_SETTING_ROUTED, "a route isn't applied");
	CHECK(!e->out_route.identity && e->out_route.src[0] == 1, "out route 1 not applied (%u)", e->out_route.src[0]);

	CHECK(queued(&fb) > 0, "nothing in flight before the restart");
	CHECK(ozzy_engine_restart(e), "restart");
	CHECK(e->settings.value[OZZY_SETTING_CLOCK_SOURCE] == OZZY_CLOCK_INTERNAL, "restart lost the setting");
	CHECK(shm.audio.settings.value[OZZY_SETTING_CLOCK_SOURCE] == OZZY_CLOCK_INTERNAL, "setting not published");
	CHECK(queued(&fb) > 0 && shm.audio.driver_ready && shm.audio.hardware_present, "not streaming after restart");

	{
		uint32_t rates[OZZY_MAX_RATES];

		CHECK(ozzy_engine_host_rates(e, rates) == 4, "DB4 host rates %u", ozzy_engine_host_rates(e, rates));
		CHECK(ozzy_engine_change_rate(e, 192000) == OZZY_SETTING_INVALID, "a rate the DB4 lacks taken");
		CHECK(ozzy_engine_change_rate(e, shm.audio.sample_rate) == OZZY_SETTING_SAME, "the running rate is a change");
		CHECK(ozzy_engine_change_rate(e, 44100) == OZZY_SETTING_RESTART, "a new rate doesn't ask for a restart");
		CHECK(ozzy_engine_restart(e) && shm.audio.sample_rate == 44100, "running at %u Hz after the rate restart",
		      (unsigned int)shm.audio.sample_rate);

		/* Locked to S/PDIF at 48 kHz (as the handshake would publish it): only that rate */
		shm.audio.settings.clock_locked = 1;
		shm.audio.settings.lock_rate = 48000;
		CHECK(ozzy_engine_host_rates(e, rates) == 1 && rates[0] == 48000, "locked host rates");
		CHECK(ozzy_engine_change_rate(e, 96000) == OZZY_SETTING_INVALID, "a rate other than the lock rate taken");
		shm.audio.settings.clock_locked = 0;
	}

	ozzy_engine_stop(e);
	CHECK(queued(&fb) == 0, "%u transfers in flight after stop", queued(&fb));
	CHECK(!shm.audio.driver_ready && !shm.audio.hardware_present, "the HAL still sees the device after stop");
	free(e);
	return CHECK_DONE("settings");
}
