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
 * hs_confirm_status - Read-modify-write the AJ input selector.
 * Writes the status byte back with bit 5 set (sign-extended into wValue),
 * which arms the device for streaming.
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
		      struct ploytec_handshake_info *info)
{
	return ploytec_handshake_eps(ops, ctx, rate, &ploytec_xone_rate_endpoints, info);
}

int ploytec_handshake_eps(const struct ploytec_host_ops *ops, void *ctx, uint32_t rate,
			  const struct ploytec_rate_endpoints *eps,
			  struct ploytec_handshake_info *info)
{
	struct ploytec_handshake_info local;
	uint32_t verify_rate = 0;
	int step = 0;
	int ret;

	if (!info)
		info = &local;

	hs_log(ops, ctx, false, "--- begin handshake sequence ---");

	hs_log(ops, ctx, false, "[%d/7] reading firmware version...", ++step);
	ret = hs_read_firmware(ops, ctx, &info->firmware);
	if (ret < 0)
		goto err;

	hs_log(ops, ctx, false, "[%d/7] reading hardware status...", ++step);
	ret = hs_read_status(ops, ctx, &info->status);
	if (ret < 0)
		goto err;

	hs_log(ops, ctx, false, "[%d/7] reading current sample rate...", ++step);
	ret = hs_read_rate(ops, ctx, &info->boot_rate);
	if (ret < 0)
		goto err;

	hs_log(ops, ctx, false, "[%d/7] setting sample rate to %u Hz...", ++step, rate);
	ret = hs_set_rate(ops, ctx, rate, eps);
	if (ret < 0)
		goto err;

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
