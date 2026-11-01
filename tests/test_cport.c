/* SPDX-License-Identifier: MIT */
/*
 * The ESI U168 XT's C-port (common/devices/ploytec/ploytec_cport.h): the control bytes of the
 * PCM out transfers, as the device gets them, decoded back into the converter writes. Checks
 * the writes a start sends for its settings, that the control bytes go idle after them in every
 * place, and that a device without the quirk only ever sends idle control bytes.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "fake_bus.h"
#include "check.h"
#include "common/devices/ploytec/ploytec_cport.h"
#include <stdlib.h>
#include <string.h>

#define PCM_OUT 0x05

struct cport_write {
	int chip;                      /* enum ploytec_cport_chip, -1: not a write */
	uint8_t reg, value;
};

struct capture {
	uint8_t bytes[8192];
	uint32_t n;
	uint32_t transfers;
	uint32_t last_busy;            /* the last transfer with a control byte that isn't idle */
};

/* The control bytes of one PCM out transfer (interrupt layout), in order */
static void take(struct capture *c, const struct fake_xfer *x)
{
	uint32_t k, subpkts = x->size / PLOYTEC_INT_OUT_SUBPKT_SIZE;

	c->transfers++;
	for (k = 0; k < subpkts; k++) {
		uint8_t b = x->buf[ploytec_out_midi_offset(PLOYTEC_OUT_LAYOUT_INT, k) + 1];

		if (b != PLOYTEC_CPORT_IDLE)
			c->last_busy = c->transfers;
		if (c->n < sizeof(c->bytes))
			c->bytes[c->n++] = b;
	}
}

/* The writes in a control byte stream: after each select byte, 16 bits as clock-low / clock-high
 * pairs, the chip told by its lines */
static uint32_t decode(const struct capture *c, struct cport_write *w, uint32_t max)
{
	uint32_t i = 0, n = 0;

	while (i + 33 <= c->n && n < max) {
		uint32_t word = 0;
		bool dac;
		int b;

		if (c->bytes[i] != PLOYTEC_CPORT_SELECT) {
			i++;
			continue;
		}
		dac = c->bytes[i + 1] == 0x1F || c->bytes[i + 1] == 0x5F;
		for (b = 0; b < 16; b++) {
			uint8_t lo = c->bytes[i + 1 + 2 * b], hi = c->bytes[i + 2 + 2 * b];

			word = word << 1 | (dac ? (hi & 0x40) != 0 : (hi & 0x02) != 0);
			CHECK((lo & (dac ? 0x20 : 0x01)) == 0 && (hi & (dac ? 0x20 : 0x01)) != 0,
			      "bit %d of write %u: clock low then high", b, n);
		}
		CHECK(((word >> 13) & 1) == 1, "write %u has R/W = 1", n);
		CHECK((word >> 14) == (dac ? 2u : 0u), "write %u: chip address %u for the %s", n, word >> 14,
		      dac ? "DAC" : "transmitter");
		w[n].chip = dac ? PLOYTEC_CPORT_DAC : PLOYTEC_CPORT_DIT;
		w[n].reg = (uint8_t)((word >> 8) & 0x1F);
		w[n].value = (uint8_t)word;
		n++;
		i += 33;
		CHECK(i < c->n && c->bytes[i] == PLOYTEC_CPORT_IDLE, "write %u ends deselected", n - 1);
	}
	return n;
}

static void run(uint16_t vid, uint16_t pid, const int (*settings)[2], struct capture *c)
{
	static const uint8_t eps[][5] = { { 0, 0x05, 3, 0xC4, 0x03 }, { 1, 0x86, 3, 0x00, 0x04 },
					  { 2, 0x83, 3, 0x40, 0 }, { 0xFF } };
	const struct ozzy_device *model = ozzy_device_lookup(vid, pid);
	static struct ozzy_shared_memory shm;
	struct fake_bus fb;
	struct fake_xfer x;
	struct ozzy_engine *e;
	unsigned int done = 0, k;

	memset(&shm, 0, sizeof(shm));
	memset(c, 0, sizeof(*c));
	fake_bus_init(&fb, &shm, false, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	for (k = 0; eps[k][0] != 0xFF; k++)
		ozzy_engine_set_endpoint(e, (enum ozzy_pipe_role)eps[k][0], eps[k][1], eps[k][2],
					 (uint16_t)(eps[k][3] | eps[k][4] << 8), 4);
	ozzy_engine_attach(e, &fb.bus, &shm);
	for (k = 0; settings && settings[k][0] >= 0; k++)
		ozzy_engine_set_setting(e, (unsigned int)settings[k][0], (uint8_t)settings[k][1]);
	CHECK(ozzy_engine_start(e), "%s starts", model->name);
	while (done < 400 && fake_bus_pop(&fb, &x)) {
		if (x.pipe == PCM_OUT) {
			take(c, &x);
			done++;
		}
		fake_bus_complete(&fb, e, &x, NULL, NULL);
	}
	ozzy_engine_stop(e);
	free(e);
}

int main(void)
{
	static const int u168[][2] = {
		{ OZZY_SETTING_SPDIF_OUT_MODE, OZZY_SPDIF_OUT_PROFESSIONAL },
		{ OZZY_SETTING_ESI_LEVEL_34, OZZY_ESI_LEVEL_MINUS10 },
		{ -1, 0 } };
	static const uint8_t att[8] = { 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0C };
	static struct capture c;
	struct cport_write w[16];
	uint32_t n, k;

	/* One write, by hand: DAC (address 2), register 4, 0xF5 */
	{
		uint8_t b[PLOYTEC_CPORT_WRITE_BYTES];
		static const uint8_t first[8] = { 0xFF, 0x7F, 0x5F, 0x7F, 0x1F, 0x3F, 0x5F, 0x7F };

		CHECK(ploytec_cport_write(b, PLOYTEC_CPORT_DAC, 0x04, 0xF5) == PLOYTEC_CPORT_WRITE_BYTES, "35 bytes");
		CHECK(memcmp(b, first, sizeof(first)) == 0, "starts deselected, selected, then 1 0 1");
		CHECK(b[PLOYTEC_CPORT_WRITE_BYTES - 1] == PLOYTEC_CPORT_IDLE, "ends deselected");
	}

	run(0x2573, 0x0012, u168, &c);
	n = decode(&c, w, 16);
	CHECK(n == 9, "U168 XT: 9 writes (got %u)", n);
	CHECK(n >= 1 && w[0].chip == PLOYTEC_CPORT_DIT && w[0].reg == 0x0D && w[0].value == 1,
	      "S/PDIF professional: transmitter channel status byte 0 = 1");
	for (k = 1; k < n && k <= 8; k++) {
		uint8_t want = (k - 1) / 2 == 1 ? 0xF5 : 0xFF;

		CHECK(w[k].chip == PLOYTEC_CPORT_DAC && w[k].reg == att[k - 1] && w[k].value == want,
		      "output %u: attenuator 0x%02X = 0x%02X (got chip %d reg 0x%02X value 0x%02X)", k,
		      att[k - 1], want, w[k].chip, w[k].reg, w[k].value);
	}
	CHECK(c.last_busy > 0 && c.last_busy < c.transfers - 64,
	      "control bytes idle again after the writes (last busy transfer %u of %u)", c.last_busy, c.transfers);

	run(0x0A4A, 0xFFDB, NULL, &c);
	CHECK(c.last_busy == 0, "Xone:DB4: only idle control bytes (busy until transfer %u)", c.last_busy);

	return CHECK_DONE("cport");
}
