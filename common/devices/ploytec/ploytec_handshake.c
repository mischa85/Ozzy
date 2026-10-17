/* SPDX-License-Identifier: MIT */
/*
 * Ploytec Vendor Handshake
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#if defined(__KERNEL__) && defined(__linux__)
#include <linux/errno.h>
#elif defined(KERNEL)
#include <sys/errno.h>
#else
#include <errno.h>
#endif

#include "ploytec_handshake.h"

/* Endpoint addresses with direction bit, for clear_halt */
#define PLOYTEC_EP_ADDR_PCM_IN   (PLOYTEC_EP_PCM_IN | 0x80)
#define PLOYTEC_EP_ADDR_PCM_OUT  PLOYTEC_EP_PCM_OUT

static void hs_log(const struct ploytec_host_ops *ops, void *ctx, bool error, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

static void hs_log(const struct ploytec_host_ops *ops, void *ctx, bool error, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	ops->vlog(ctx, error, fmt, args);
	va_end(args);
}

/*
 * hs_read_firmware - Vendor request 'V': firmware version.
 */
static int hs_read_firmware(const struct ploytec_host_ops *ops, void *ctx,
			    struct ploytec_firmware_version *fw)
{
	uint8_t buf[15] = { 0 };
	int ret;

	ret = ops->control(ctx, PLOYTEC_REQTYPE_VENDOR_IN, PLOYTEC_CMD_FIRMWARE,
			   0, 0, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	*fw = ploytec_parse_firmware(buf);
	hs_log(ops, ctx, false, "firmware: v1.%u.%u (chip ID: 0x%02X)",
	       fw->major, fw->minor, fw->chip_id);
	return 0;
}

/*
 * hs_read_status - Vendor request 'I', wIndex 0: AJ input selector.
 * See ploytec_aj_input_status for confirmed vs partially understood bits.
 */
static int hs_read_status(const struct ploytec_host_ops *ops, void *ctx, uint8_t *status)
{
	struct ploytec_aj_input_status st;
	uint8_t buf[1] = { 0 };
	int ret;

	ret = ops->control(ctx, PLOYTEC_REQTYPE_VENDOR_IN, PLOYTEC_CMD_STATUS,
			   0, PLOYTEC_REG_AJ_INPUT_SEL, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	*status = buf[0];
	ploytec_decode_aj_input(buf[0], &st);
	hs_log(ops, ctx, false, "status: 0x%02X [InputSel:%d] [DigLock:%d] [mode: %d%d%d%d%d%d]",
	       st.raw, st.input_select, st.digital_lock,
	       st.mode7, st.mode6, st.mode5, st.mode4, st.mode3, st.mode0);
	return 0;
}

/*
 * hs_read_rate - UAC1 endpoint GET_CUR: current sample rate.
 */
static int hs_read_rate(const struct ploytec_host_ops *ops, void *ctx, uint32_t *rate)
{
	uint8_t buf[3] = { 0 };
	int ret;

	ret = ops->control(ctx, PLOYTEC_CMD_GET_RATE_TYPE, PLOYTEC_CMD_GET_RATE_REQ,
			   0x0100, 0, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	*rate = ploytec_decode_rate(buf);
	hs_log(ops, ctx, false, "hardware sample rate: %u Hz (raw: %02X %02X %02X)",
	       *rate, buf[0], buf[1], buf[2]);
	return 0;
}

/*
 * hs_set_rate - UAC1 endpoint SET_CUR to the PCM IN, then the PCM OUT endpoint.
 */
static int hs_set_rate(const struct ploytec_host_ops *ops, void *ctx, uint32_t rate,
		       const struct ploytec_rate_endpoints *eps)
{
	uint8_t buf[3];
	int ret;

	ploytec_encode_rate(rate, buf);
	ret = ops->control(ctx, PLOYTEC_CMD_SET_RATE_TYPE, PLOYTEC_CMD_SET_RATE_REQ,
			   0x0100, eps->pcm_in, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	ploytec_encode_rate(rate, buf);
	return ops->control(ctx, PLOYTEC_CMD_SET_RATE_TYPE, PLOYTEC_CMD_SET_RATE_REQ,
			    0x0100, eps->pcm_out, buf, sizeof(buf));
}

/*
 * hs_confirm_status - Read-modify-write the AJ input selector, as the official
 * setAJDMAInputChannels does at every stream start (bulkAudioRun): writes the status
 * byte back with bits 5 and 4 set (sign-extended into wValue), which arms the
 * device for streaming. Sent to every device.
 */
static int hs_confirm_status(const struct ploytec_host_ops *ops, void *ctx)
{
	uint8_t buf[1] = { 0 };
	uint16_t wvalue;
	int ret;

	ret = ops->control(ctx, PLOYTEC_REQTYPE_VENDOR_IN, PLOYTEC_CMD_STATUS,
			   0, PLOYTEC_REG_AJ_INPUT_SEL, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	wvalue = ploytec_confirm_wvalue(buf[0]);
	ret = ops->control(ctx, PLOYTEC_REQTYPE_VENDOR_OUT, PLOYTEC_CMD_STATUS,
			   wvalue, PLOYTEC_REG_AJ_INPUT_SEL, 0, 0);
	if (ret < 0)
		return ret;

	hs_log(ops, ctx, false, "status confirmed (read=0x%02X, wrote wValue=0x%04X)",
	       buf[0], wvalue);
	return 0;
}

/*
 * hs_read_selector - 'I' wIndex 0, read again after 20 ms while the digital lock
 * bit is set (both selector routines of the official driver do this).
 */
static int hs_read_selector(const struct ploytec_host_ops *ops, void *ctx, uint8_t *val)
{
	int ret;

	ret = ops->control(ctx, PLOYTEC_REQTYPE_VENDOR_IN, PLOYTEC_CMD_STATUS,
			   0, PLOYTEC_REG_AJ_INPUT_SEL, val, 1);
	if (ret >= 0 && (*val & PLOYTEC_AJ_DIGITAL_LOCK_BIT)) {
		ops->sleep_ms(ctx, 20);
		ret = ops->control(ctx, PLOYTEC_REQTYPE_VENDOR_IN, PLOYTEC_CMD_STATUS,
				   0, PLOYTEC_REG_AJ_INPUT_SEL, val, 1);
	}
	return ret;
}

/*
 * hs_follow_lock - With the S/PDIF clock source: read the selector again and, if the
 * device reports lock, the rate it locked to. The official updateAjInputSelector tries
 * up to 20 times while the rate reads 192000: it takes 192k only on a device with a
 * 192k alt setting, which no DIGITAL_IN device has, so a lasting 192000 counts as no
 * lock here. @clock is cleared first.
 */
static int hs_follow_lock(const struct ploytec_host_ops *ops, void *ctx,
			  struct ozzy_clock_status *clock)
{
	uint8_t buf[1] = { 0 };
	uint32_t rate = 0;
	int i, ret;

	clock->locked = false;
	clock->lock_rate = 0;

	ret = hs_read_selector(ops, ctx, buf);
	if (ret < 0)
		return ret;
	if (!ploytec_aj_locked(buf[0])) {
		hs_log(ops, ctx, false, "clock source S/PDIF: no lock (selector 0x%02X)", buf[0]);
		return 0;
	}

	for (i = 0; i < 20; i++) {
		ret = hs_read_rate(ops, ctx, &rate);
		if (ret < 0)
			return ret;
		if (rate != 192000)
			break;
		ops->sleep_ms(ctx, 10);
	}
	if (rate == 192000 || rate == 0) {
		hs_log(ops, ctx, true, "clock source S/PDIF: locked, but the rate reads %u Hz", rate);
		return 0;
	}

	clock->locked = true;
	clock->lock_rate = rate;
	hs_log(ops, ctx, false, "clock source S/PDIF: locked at %u Hz", rate);
	return 0;
}

/*
 * hs_selector - The generic updateAjInputSelector of the official configurationDone:
 * bit 1 from the clock source (ploytec_aj_clock_byte), bit 0 cleared on OZZY_QUIRK_AJ_BIT0
 * devices, and the byte written back only if it changed, then wait 40 ms and read the
 * rate (the device may re-clock). The Xones report bit 1 set at power-up, so at the
 * default (Internal) they get no write. With S/PDIF, @clock gets the lock state and rate.
 */
static int hs_selector(const struct ploytec_host_ops *ops, void *ctx, uint32_t quirks,
		       uint8_t clock_source, struct ozzy_clock_status *clock)
{
	uint8_t buf[1] = { 0 };
	uint32_t rate;
	uint8_t want;
	int ret;

	ops->sleep_ms(ctx, 5);
	ret = hs_read_selector(ops, ctx, buf);
	if (ret < 0)
		return ret;

	want = ploytec_aj_clock_byte(buf[0], clock_source);
	if (quirks & OZZY_QUIRK_AJ_BIT0)
		want &= (uint8_t)~PLOYTEC_AJ_MODE0_BIT;
	if (want != buf[0]) {
		ret = ops->control(ctx, PLOYTEC_REQTYPE_VENDOR_OUT, PLOYTEC_CMD_STATUS,
				   (uint16_t)(int16_t)(int8_t)want, PLOYTEC_REG_AJ_INPUT_SEL, 0, 0);
		if (ret < 0)
			return ret;
		ops->sleep_ms(ctx, 40);
		hs_log(ops, ctx, false, "selector: read 0x%02X, wrote 0x%02X", buf[0], want);
		if (clock_source == OZZY_CLOCK_INTERNAL)
			return hs_read_rate(ops, ctx, &rate);
	}
	if (clock_source != OZZY_CLOCK_INTERNAL)
		return hs_follow_lock(ops, ctx, clock);
	return 0;
}

/*
 * hs_digital_out_sel - writeDigitalOutSelector: 'I' wIndex 2 = AjExtData[4] (default 0),
 * no read first.
 */
static int hs_digital_out_sel(const struct ploytec_host_ops *ops, void *ctx)
{
	return ops->control(ctx, PLOYTEC_REQTYPE_VENDOR_OUT, PLOYTEC_CMD_STATUS,
			    0, PLOYTEC_REG_DIGITAL_OUT_SEL, 0, 0);
}

/*
 * hs_spdif_regs - The S/PDIF output registers 0x0D..0x11 writeDigitalOutMode wants for
 * @mode (AjExtData[3], enum ozzy_spdif_out_mode) at @rate, from the ones read (@old):
 *   mode 0 (default, consumer): 0x0D = 0x04, 0x0E = 0, 0x0F = 0, 0x10 = (old & 0xF0) | rate
 *     code (IEC 60958: 32k 3, 44.1k 0, 48k 2, 88.2k 8, 96k 0xA, 176.4k 0xC, 192k 0xE),
 *     0x11 = 0x0B
 *   mode 1 (professional): 0x0D = 0xC1 / 0x41 / 0x81 at 32k / 44.1k / 48k, 0x01 above (other
 *     rates: unchanged), 0x0E = 0, 0x0F = 0x2C, 0x10 = 0, 0x11 = 0x50 / 0x10 / 0x58 / 0x18
 *     at 88.2k / 96k / 176.4k / 192k, else 0
 */
static void hs_spdif_regs(uint8_t mode, uint32_t rate, const uint8_t *old, uint8_t *want)
{
	uint8_t code;

	want[1] = 0x00;
	if (mode == OZZY_SPDIF_OUT_PROFESSIONAL) {
		switch (rate) {
		case 32000:  want[0] = 0xC1; want[4] = 0x00; break;
		case 44100:  want[0] = 0x41; want[4] = 0x00; break;
		case 48000:  want[0] = 0x81; want[4] = 0x00; break;
		case 88200:  want[0] = 0x01; want[4] = 0x50; break;
		case 96000:  want[0] = 0x01; want[4] = 0x10; break;
		case 176400: want[0] = 0x01; want[4] = 0x58; break;
		case 192000: want[0] = 0x01; want[4] = 0x18; break;
		default:     want[0] = old[0]; want[4] = 0x00; break;
		}
		want[2] = 0x2C;
		want[3] = 0x00;
		return;
	}

	switch (rate) {
	case 32000:  code = 0x3; break;
	case 48000:  code = 0x2; break;
	case 88200:  code = 0x8; break;
	case 96000:  code = 0xA; break;
	case 176400: code = 0xC; break;
	case 192000: code = 0xE; break;
	default:     code = 0x0; break;  /* 44.1k */
	}
	want[0] = 0x04;
	want[2] = 0x00;
	want[3] = (old[3] & 0xF0) | code;
	want[4] = 0x0B;
}

/*
 * hs_spdif_out - writeDigitalOutMode: read the S/PDIF output registers 0x0D..0x11 and write
 * each one that differs from what @mode wants at @rate (hs_spdif_regs). The official
 * startStreaming does this right after the first SET_CUR pair, and setDigitalOutMode with
 * streaming suspended.
 */
static int hs_spdif_out(const struct ploytec_host_ops *ops, void *ctx, uint32_t rate, uint8_t mode)
{
	uint8_t old[PLOYTEC_SPDIF_REGS] = { 0 };
	uint8_t want[PLOYTEC_SPDIF_REGS];
	int i, ret;

	ret = ops->control(ctx, PLOYTEC_REQTYPE_VENDOR_IN, PLOYTEC_CMD_ESI_A,
			   PLOYTEC_SPDIF_REG_FIRST << 8, PLOYTEC_ESI_A_INDEX, old, sizeof(old));
	if (ret < 0)
		return ret;

	hs_spdif_regs(mode, rate, old, want);

	for (i = 0; i < PLOYTEC_SPDIF_REGS; i++) {
		if (old[i] == want[i])
			continue;
		ret = ops->control(ctx, PLOYTEC_REQTYPE_VENDOR_OUT, PLOYTEC_CMD_ESI_A,
				   (uint16_t)((PLOYTEC_SPDIF_REG_FIRST + i) << 8 | want[i]),
				   PLOYTEC_ESI_A_INDEX, 0, 0);
		if (ret < 0)
			return ret;
	}
	hs_log(ops, ctx, false, "S/PDIF out registers set for %u Hz (%s)", rate,
	       ozzy_spdif_out_mode_names[mode]);
	return 0;
}

/*
 * hs_esi_a_request - ESI 'A' write (sendEsuARequest), no data phase.
 */
static int hs_esi_a_request(const struct ploytec_host_ops *ops, void *ctx, uint16_t wvalue)
{
	int ret;

	ret = ops->control(ctx, PLOYTEC_REQTYPE_VENDOR_OUT, PLOYTEC_CMD_ESI_A,
			   wvalue, PLOYTEC_ESI_A_INDEX, 0, 0);
	if (ret < 0)
		hs_log(ops, ctx, true, "ESI 'A' request 0x%03X failed (ret=%d)", wvalue, ret);
	return ret;
}

/*
 * hs_esi_setup - The ESI U-series selector requests of the official configurationDone,
 * which runs before startStreaming sets the rate:
 *   - U168 XT only: 'A' 0x349
 *   - ESI input selector (updateAjInputSelector_Esu186) in place of hs_selector:
 *     read 'I' wIndex 0 (again after 20 ms while the lock bit is set), keep the
 *     PRESERVE_MASK bits, set STATE_A (the driver's default), send 'A' 0x348, and
 *     write the byte back only if it changed, then wait 200 ms (the device
 *     re-clocks; the official driver reads the rate back next, as the handshake does)
 * The second 'A' request overrides the first, as in the official driver, which
 * runs the selector again after the U168 write.
 */
static int hs_esi_setup(const struct ploytec_host_ops *ops, void *ctx, uint32_t quirks)
{
	uint8_t buf[1] = { 0 };
	uint8_t want;
	int ret;

	if (quirks & OZZY_QUIRK_ESI_U168) {
		ret = hs_esi_a_request(ops, ctx, PLOYTEC_ESI_A_U168);
		if (ret < 0)
			return ret;
	}

	ret = hs_read_selector(ops, ctx, buf);
	if (ret < 0)
		return ret;

	want = (buf[0] & PLOYTEC_AJ_ESU_PRESERVE_MASK) | PLOYTEC_AJ_ESU_STATE_A;
	ret = hs_esi_a_request(ops, ctx, PLOYTEC_ESI_A_BASE);
	if (ret < 0)
		return ret;

	if (want != buf[0]) {
		ret = ops->control(ctx, PLOYTEC_REQTYPE_VENDOR_OUT, PLOYTEC_CMD_STATUS,
				   (uint16_t)(int16_t)(int8_t)want, PLOYTEC_REG_AJ_INPUT_SEL, 0, 0);
		if (ret < 0)
			return ret;
		ops->sleep_ms(ctx, 200);
	}
	hs_log(ops, ctx, false, "ESI selector: read 0x%02X, now 0x%02X", buf[0], want);
	return 0;
}

/*
 * hs_esi_cpld - The CPLD byte, 'I' wIndex 1, wValue 0xE7 (setEsuCpldByte(0)), late in
 * the official configurationDone, after the selectors (ESI U-series and ESU1808).
 */
static int hs_esi_cpld(const struct ploytec_host_ops *ops, void *ctx)
{
	int ret;

	ret = ops->control(ctx, PLOYTEC_REQTYPE_VENDOR_OUT, PLOYTEC_CMD_STATUS,
			   PLOYTEC_ESI_CPLD_BASE, PLOYTEC_REG_US3XX_CONFIG, 0, 0);
	if (ret >= 0)
		hs_log(ops, ctx, false, "ESI CPLD byte 0x%02X written", PLOYTEC_ESI_CPLD_BASE);
	return ret;
}

/*
 * hs_clear_pcm_halt - Reset both PCM endpoints before streaming starts.
 * The A&H Windows driver does this after the status confirm (abort + clear
 * ENDPOINT_HALT on EP 0x86, then EP 0x05), seen in captures of the Xone:DB2 and
 * Xone:4D. Without it the 4D never starts streaming on Linux (#75).
 */
static int hs_clear_pcm_halt(const struct ploytec_host_ops *ops, void *ctx)
{
	int ret;

	ops->sleep_ms(ctx, 500);
	ret = ops->clear_halt(ctx, PLOYTEC_EP_ADDR_PCM_IN);
	if (ret < 0) {
		hs_log(ops, ctx, true, "clear halt on PCM in failed (ret=%d)", ret);
		return ret;
	}
	ret = ops->clear_halt(ctx, PLOYTEC_EP_ADDR_PCM_OUT);
	if (ret < 0)
		hs_log(ops, ctx, true, "clear halt on PCM out failed (ret=%d)", ret);
	return ret;
}

/* Positional: the macOS kext compiles this file as C++11 */
const struct ploytec_rate_endpoints ploytec_xone_rate_endpoints = { PLOYTEC_EP_RATE_IN, PLOYTEC_EP_RATE_OUT };
const struct ploytec_rate_endpoints ploytec_isoc_rate_endpoints = { PLOYTEC_EP_RATE_IN, PLOYTEC_ISOC_EP_RATE_OUT };

int ploytec_handshake(const struct ploytec_host_ops *ops, void *ctx, uint32_t rate,
		      uint32_t quirks, const struct ozzy_settings *settings,
		      struct ploytec_handshake_info *info)
{
	return ploytec_handshake_eps(ops, ctx, rate, &ploytec_xone_rate_endpoints, quirks,
				     settings, info);
}

int ploytec_handshake_eps(const struct ploytec_host_ops *ops, void *ctx, uint32_t rate,
			  const struct ploytec_rate_endpoints *eps, uint32_t quirks,
			  const struct ozzy_settings *settings,
			  struct ploytec_handshake_info *info)
{
	struct ploytec_handshake_info local;
	uint8_t clock_source = OZZY_CLOCK_INTERNAL;
	uint8_t spdif_mode = OZZY_SPDIF_OUT_CONSUMER;
	uint32_t verify_rate = 0;
	int step = 0;
	int ret = 0;

	if (!info)
		info = &local;
	info->clock.locked = false;
	info->clock.lock_rate = 0;
	if (settings && ozzy_has_setting(quirks, OZZY_SETTING_CLOCK_SOURCE))
		clock_source = settings->value[OZZY_SETTING_CLOCK_SOURCE];
	if (settings && ozzy_has_setting(quirks, OZZY_SETTING_SPDIF_OUT_MODE))
		spdif_mode = settings->value[OZZY_SETTING_SPDIF_OUT_MODE];

	hs_log(ops, ctx, false, "--- begin handshake sequence ---");

	hs_log(ops, ctx, false, "[%d/7] reading firmware version...", ++step);
	ret = hs_read_firmware(ops, ctx, &info->firmware);
	if (ret < 0)
		goto err;

	/* The official driver reads the firmware first (onServiceStart), then waits in configurationDone */
	if (quirks & OZZY_QUIRK_WAIT_200MS)
		ops->sleep_ms(ctx, 200);
	else if (quirks & OZZY_QUIRK_WAIT_100MS)
		ops->sleep_ms(ctx, 100);

	hs_log(ops, ctx, false, "[%d/7] reading hardware status...", ++step);
	ret = hs_read_status(ops, ctx, &info->status);
	if (ret < 0)
		goto err;

	hs_log(ops, ctx, false, "[%d/7] reading current sample rate...", ++step);
	ret = hs_read_rate(ops, ctx, &info->boot_rate);
	if (ret < 0)
		goto err;

	/* configurationDone: the input selector, then the digital output selector */
	if (quirks & OZZY_QUIRK_ESI) {
		hs_log(ops, ctx, false, "ESI selector and 'A' request...");
		ret = hs_esi_setup(ops, ctx, quirks);
	} else if (!(quirks & OZZY_QUIRK_NO_AJ_SELECTOR)) {
		ret = hs_selector(ops, ctx, quirks, clock_source, &info->clock);
	}
	if (ret < 0)
		goto err;
	/* Locked to S/PDIF: the official driver adopts the lock rate and offers only it */
	if (info->clock.locked && info->clock.lock_rate != rate) {
		hs_log(ops, ctx, false, "following the S/PDIF lock: %u Hz instead of %u Hz",
		       info->clock.lock_rate, rate);
		rate = info->clock.lock_rate;
	}
	info->rate = rate;
	if (quirks & OZZY_QUIRK_DIGITAL_OUT_SEL) {
		ret = hs_digital_out_sel(ops, ctx);
		if (ret < 0)
			goto err;
	}
	if (quirks & OZZY_QUIRK_ESI_CPLD) {
		ret = hs_esi_cpld(ops, ctx);
		if (ret < 0)
			goto err;
	}

	hs_log(ops, ctx, false, "[%d/7] setting sample rate to %u Hz...", ++step, rate);
	ret = hs_set_rate(ops, ctx, rate, eps);
	if (ret < 0)
		goto err;

	if (quirks & OZZY_QUIRK_SPDIF_OUT) {
		ret = hs_spdif_out(ops, ctx, rate, spdif_mode);
		if (ret < 0)
			goto err;
	}

	/* The official driver reads the rate back and waits 50 ms (bulkAudioRun) */
	hs_log(ops, ctx, false, "[%d/7] verifying sample rate...", ++step);
	ret = hs_read_rate(ops, ctx, &verify_rate);
	if (ret < 0)
		goto err;
	if (verify_rate != rate) {
		hs_log(ops, ctx, true, "device runs at %u Hz instead of %u Hz", verify_rate, rate);
		ret = -EINVAL;
		goto err;
	}
	ops->sleep_ms(ctx, 50);

	hs_log(ops, ctx, false, "[%d/7] reading hardware status...", ++step);
	ret = hs_read_status(ops, ctx, &info->status);
	if (ret < 0)
		goto err;

	hs_log(ops, ctx, false, "[%d/7] confirming device status...", ++step);
	ret = hs_confirm_status(ops, ctx);
	if (ret < 0)
		goto err;

	if (ops->clear_halt) {
		ret = hs_clear_pcm_halt(ops, ctx);
		if (ret < 0)
			goto err;
	}

	hs_log(ops, ctx, false, "--- handshake complete, device ready ---");
	return 0;

err:
	hs_log(ops, ctx, true, "handshake failed at step %d (ret=%d)", step, ret);
	return ret;
}
