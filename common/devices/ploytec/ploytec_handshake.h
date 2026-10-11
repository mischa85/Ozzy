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
 * ploytec_handshake - Arm the device for streaming at @rate Hz.
 * @info may be NULL. Returns 0 on success, negative errno on failure.
 */
int ploytec_handshake(const struct ploytec_host_ops *ops, void *ctx, uint32_t rate,
		      struct ploytec_handshake_info *info);

#ifdef __cplusplus
}
#endif

#endif /* OZZY_PLOYTEC_HANDSHAKE_H */
