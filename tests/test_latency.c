/* SPDX-License-Identifier: MIT */
/*
 * ozzy_device_latency and the latency column of ozzy_devices.def: the DB4's measured frames per
 * rate and direction, 0 for a rate the row does not list, a device without a measurement or no
 * device, and no row with more pairs than rates (a pair too many would shift none of them, but
 * means the row is not what its author thought).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "check.h"   /* first: ozzy_devices.h needs NULL from elsewhere */
#include "../common/ozzy_devices.h"

int main(void)
{
	const struct ozzy_device *db4 = ozzy_device_lookup(0x0A4A, 0xFFDB);
	const struct ozzy_device *db2 = ozzy_device_lookup(0x0A4A, 0xFFD2);
	uint32_t hz[OZZY_MAX_RATES];
	unsigned int d, i;

	CHECK(db4 && db2, "lookup");
	CHECK(ozzy_device_latency(db4, 44100, false) == 155, "DB4 44.1k out");
	CHECK(ozzy_device_latency(db4, 44100, true) == 22, "DB4 44.1k in");
	CHECK(ozzy_device_latency(db4, 48000, false) == 157, "DB4 48k out");
	CHECK(ozzy_device_latency(db4, 88200, true) == 107, "DB4 88.2k in");
	CHECK(ozzy_device_latency(db4, 96000, false) == 226, "DB4 96k out");
	CHECK(ozzy_device_latency(db4, 96000, true) == 110, "DB4 96k in");
	CHECK(ozzy_device_latency(db4, 192000, false) == 0, "a rate the row does not list");
	CHECK(ozzy_device_latency(db2, 48000, false) == 0, "a device not measured");
	CHECK(ozzy_device_latency(0, 48000, true) == 0, "no device");

	for (d = 0; d < OZZY_NUM_DEVICES; d++) {
		const struct ozzy_device *dev = &ozzy_devices[d];

		for (i = ozzy_device_rates(dev, hz); i < OZZY_MAX_RATES; i++)
			CHECK(!dev->latency[2 * i] && !dev->latency[2 * i + 1], "%s: latency pair %u has no rate", dev->key, i);
	}
	return CHECK_DONE("latency");
}
