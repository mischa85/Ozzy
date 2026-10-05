/* SPDX-License-Identifier: MIT */
/*
 * Ozzy USB Audio Driver - PCM Subsystem
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_PCM_H
#define OZZY_PCM_H

struct ozzy_chip;

#define OZZY_PCM_N_URBS  4

struct pcm_urb {
	struct ozzy_chip *chip;
	struct urb instance;
	struct usb_anchor submitted;
	uint8_t *buffer;
};

struct pcm_substream {
	spinlock_t lock;
	struct snd_pcm_substream *instance;

	bool active;

	snd_pcm_uframes_t dma_off;     /* current position in ALSA DMA area (bytes) */
	snd_pcm_uframes_t period_off;  /* current position within current period */
};

/* PCM streaming states */
enum {
	STREAM_DISABLED,  /* no PCM streaming */
	STREAM_STARTING,  /* PCM streaming requested, waiting to become ready */
	STREAM_RUNNING,   /* PCM streaming active */
	STREAM_STOPPING   /* PCM streaming shutting down */
};

/*
 * struct pcm_runtime - Per-device PCM subsystem state.
 */
struct pcm_runtime {
	struct ozzy_chip *chip;
	struct snd_pcm *instance;

	struct pcm_substream playback;
	struct pcm_substream capture;
	bool panic;  /* if set, driver won't do any more PCM on this device */

	struct pcm_urb pcm_out_urbs[OZZY_PCM_N_URBS];
	struct pcm_urb pcm_in_urbs[OZZY_PCM_N_URBS];

	struct mutex stream_mutex;
	uint8_t stream_state;  /* one of STREAM_xxx */
	uint8_t rate;          /* index into device's rate table */
};

/*
 * ozzy_pcm_init - Create and initialize the PCM subsystem.
 * Allocates pcm_runtime, registers ALSA PCM, sets up URBs.
 * Returns 0 on success, negative errno on failure.
 */
int ozzy_pcm_init(struct ozzy_chip *chip);

/*
 * ozzy_pcm_init_urbs - (Re)initialize and submit PCM URBs.
 * Called during initial probe and after USB device reset.
 * Returns 0 on success, negative errno on failure.
 */
int ozzy_pcm_init_urbs(struct ozzy_chip *chip);

/*
 * ozzy_pcm_abort - Emergency stop all PCM URBs.
 * Called during disconnect and pre-reset. Sets panic flag.
 */
void ozzy_pcm_abort(struct ozzy_chip *chip);

/*
 * ozzy_pcm_drain - Stop resubmitting and let in-flight URBs complete.
 * Call before ozzy_pcm_abort when the device stays connected.
 */
void ozzy_pcm_drain(struct ozzy_chip *chip);

#endif /* OZZY_PCM_H */
