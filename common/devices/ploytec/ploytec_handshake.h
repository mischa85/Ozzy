/* SPDX-License-Identifier: MIT */
/*
 * Ploytec Vendor Handshake
 *
 * The control-transfer sequence that arms a Ploytec device for streaming.
 * The sequence lives here once; each platform only supplies the host
 * services in struct ploytec_host_ops (control transfers, sleeping, logging).
 *
 * Used by both Linux (kernel module) and macOS (kext) drivers.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_PLOYTEC_HANDSHAKE_H
#define OZZY_PLOYTEC_HANDSHAKE_H

#if defined(__KERNEL__) && defined(__linux__)
#include <linux/types.h>
#include <linux/stdarg.h>
#else
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#endif

#include "ploytec_protocol.h"
#include "../../ozzy_quirks.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * struct ploytec_host_ops - Platform services used by ploytec_handshake().
 * @control:    EP0 control transfer. @data may live on the caller's stack, so
 *              copy it if the platform needs DMA-safe memory. Returns >= 0 on
 *              success, negative errno on failure.
 * @clear_halt: clear ENDPOINT_HALT on @endpoint (address incl. direction bit).
 *              NULL skips the step.
 * @sleep_ms:   sleep for @ms milliseconds.
 * @vlog:       log one line (no trailing newline in @fmt). @error marks failures.
 */
struct ploytec_host_ops {
	int  (*control)(void *ctx, uint8_t request_type, uint8_t request,
			uint16_t value, uint16_t index, void *data, uint16_t length);
	int  (*clear_halt)(void *ctx, uint8_t endpoint);
	void (*sleep_ms)(void *ctx, unsigned int ms);
	void (*vlog)(void *ctx, bool error, const char *fmt, va_list args);
};

/*
 * struct ploytec_handshake_info - What the device reported during the handshake.
 */
struct ploytec_handshake_info {
	struct ploytec_firmware_version firmware;
	uint32_t boot_rate;  /* rate before SET_CUR (the DB2 boots at 44100) */
	uint8_t status;      /* AJ input selector byte as read before the confirm write */
};

/*
 * struct ploytec_rate_endpoints - wIndex of the two UAC1 SET_CUR rate requests:
 * the PCM IN and PCM OUT endpoint addresses. The official driver sends them to
 * whichever pipes it streams on (PGDevice::startStreaming).
 */
struct ploytec_rate_endpoints {
	uint16_t pcm_in;
	uint16_t pcm_out;
};

extern const struct ploytec_rate_endpoints ploytec_xone_rate_endpoints;  /* 0x86, 0x05 */
extern const struct ploytec_rate_endpoints ploytec_isoc_rate_endpoints;  /* 0x86, 0x02 */

/*
 * ploytec_handshake - Arm a Xone (bulk/interrupt out) device for streaming at @rate Hz.
 * @quirks: the device's enum ozzy_quirk bits (the handshake uses the ones that add,
 * replace or skip setup requests: NO_AJ_SELECTOR, ESI, ESI_U168, ESI_CPLD, WAIT_200MS,
 * WAIT_100MS, AJ_BIT0, SPDIF_OUT, DIGITAL_OUT_SEL). @info may be NULL. Returns 0 on success,
 * negative errno on failure.
 */
int ploytec_handshake(const struct ploytec_host_ops *ops, void *ctx, uint32_t rate,
		      uint32_t quirks, struct ploytec_handshake_info *info);

/*
 * ploytec_handshake_eps - The same sequence with the rate requests sent to @eps.
 * Bulk in / isoc out devices (Akai EIE Pro) use ploytec_isoc_rate_endpoints and
 * no clear_halt (the official driver doesn't clear halts, and the Xone endpoints
 * it would hit are not the EIE's PCM pipes).
 */
int ploytec_handshake_eps(const struct ploytec_host_ops *ops, void *ctx, uint32_t rate,
			  const struct ploytec_rate_endpoints *eps, uint32_t quirks,
			  struct ploytec_handshake_info *info);

#ifdef __cplusplus
}
#endif

#endif /* OZZY_PLOYTEC_HANDSHAKE_H */
