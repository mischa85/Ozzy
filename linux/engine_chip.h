/* SPDX-License-Identifier: MIT */
/*
 * Ozzy USB Audio Driver - Linux on the shared engine (make OZZY_ENGINE=1)
 *
 * The engine (engine/) runs the device's protocol, as in the macOS kext and the Windows
 * driver; this side gives it a bus (engine_bus.c: URBs, control requests, a timer), ALSA PCM
 * on its rings (engine_pcm.c), rawmidi on its MIDI rings (engine_midi.c) and the settings
 * as ALSA controls (engine_controls.c). engine_core.c probes the device.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_ENGINE_CHIP_H
#define OZZY_ENGINE_CHIP_H

#include <linux/usb.h>
#include <linux/hrtimer.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/rawmidi.h>

#include "../engine/core/ozzy_engine.h"
#include "../common/midi_running_status.h"

#define ECHIP_MAX_INTERFACES  4
#define LBUS_MAX_PIPES        OZZY_PIPE_ROLES
#define LBUS_URBS             8     /* per pipe: more than any engine keeps in flight (4) */
#define LBUS_PCM_SLOTS        4     /* PCM transfer buffers per direction: the transfers in flight */

#define echip_log(chip, fmt, ...)  dev_info(&(chip)->dev->dev, "[ozzy] " fmt "\n", ##__VA_ARGS__)
#define echip_err(chip, fmt, ...)  dev_err(&(chip)->dev->dev, "[ozzy] " fmt "\n", ##__VA_ARGS__)

struct ozzy_echip;

/* One URB of a pipe and what its completion needs */
struct lbus_urb {
	struct urb *urb;
	struct ozzy_echip *chip;
	uint8_t pipe;                      /* index in lbus.pipes */
	uint8_t index;                     /* bit in the pipe's busy mask */
	void *xfer;                        /* the engine's transfer cookie */
	struct ozzy_isoc_packet *packets;  /* isochronous: the engine's packets */
};

struct lbus_pipe {
	uint8_t address;                   /* bEndpointAddress */
	uint8_t type;                      /* USB_ENDPOINT_XFER_xxx */
	uint8_t interval;                  /* bInterval */
	unsigned int usb_pipe;
	struct lbus_urb urbs[LBUS_URBS];
	unsigned long busy;                /* bit per URB in flight */
};

/*
 * struct lbus - The engine's bus (engine/core/ozzy_bus.h) on URBs. Completions run in
 * interrupt context; the engine resubmits from there with GFP_ATOMIC.
 * @in_completion: completions running now (lbus_wait_completions)
 */
struct lbus {
	struct ozzy_bus bus;
	struct ozzy_echip *chip;
	struct usb_device *dev;
	struct lbus_pipe pipes[LBUS_MAX_PIPES];
	unsigned int num_pipes;
	struct usb_anchor anchor;          /* every URB in flight */
	atomic_t in_completion;
	struct hrtimer timer;
	ktime_t timer_interval;
	bool timer_on;
	u8 *ctrl_buf;                      /* control requests: the engine passes stack buffers */
	struct ozzy_engine_memory mem;     /* the transfer buffers, each its own kmalloc */
};

/* One PCM direction: ALSA's buffer as the engine's ring */
struct estream {
	struct snd_pcm_substream *substream;
	struct ozzy_pcm_ring ring;
	spinlock_t lock;
	bool attached;
	uint64_t pos;                      /* ring position the engine reported last */
	uint64_t period_next;              /* ring position of the next period boundary */
};

struct emidi {
	struct snd_rawmidi *rmidi;
	struct snd_rawmidi_substream *in;  /* triggered input, NULL if none */
	struct snd_rawmidi_substream *out; /* triggered output, NULL if none */
	spinlock_t lock;
	struct midi_running_status rs;     /* the output stream's running status (PR #80) */
	u8 pending[2];                     /* expanded bytes that didn't fit the ring yet */
	unsigned int pending_count;
};

/*
 * struct ozzy_echip - One device (snd_card private data).
 * @lock: serializes restarts (rate changes, settings) with each other and disconnect
 */
struct ozzy_echip {
	struct usb_device *dev;
	struct snd_card *card;
	struct usb_interface *intf[ECHIP_MAX_INTERFACES];   /* [0] probed, the rest claimed */
	unsigned int num_intf;
	const struct ozzy_device *model;
	struct ozzy_engine *engine;
	struct ozzy_shared_memory *shm;    /* CPU only (vzalloc): the transfers use lbus.mem */
	struct lbus bus;
	struct mutex lock;
	bool running;                      /* engine started, device present */
	bool gone;                         /* disconnected */
	struct snd_pcm *pcm;
	struct estream stream[OZZY_PCM_DIRS];
	struct emidi midi;
	struct snd_kcontrol *clock_lock_kctl;
};

/* engine_bus.c */
int lbus_init(struct lbus *bus, struct ozzy_echip *chip);
int lbus_add_pipe(struct lbus *bus, const struct usb_endpoint_descriptor *epd);
int lbus_alloc_memory(struct lbus *bus, const struct ozzy_engine *engine);
void lbus_kill(struct lbus *bus);
void lbus_wait_completions(struct lbus *bus);
void lbus_free(struct lbus *bus);

/* engine_core.c */
int echip_restart(struct ozzy_echip *chip);

/* engine_pcm.c */
int epcm_init(struct ozzy_echip *chip);
void epcm_progress(struct ozzy_echip *chip, enum ozzy_pcm_dir dir, uint64_t frames);
void epcm_stop_streams(struct ozzy_echip *chip);

/* engine_midi.c */
int emidi_init(struct ozzy_echip *chip);
void emidi_service(struct ozzy_echip *chip);

/* engine_controls.c */
int econtrols_init(struct ozzy_echip *chip);

#endif /* OZZY_ENGINE_CHIP_H */
