/* SPDX-License-Identifier: MIT */
/*
 * Ozzy USB Audio Driver - Linux on the shared engine
 *
 * The engine (engine/) runs the device's protocol, as in the macOS kext and the Windows
 * driver; this side gives it a bus (bus.c: URBs, control requests, a timer), ALSA PCM on its
 * rings (pcm.c), rawmidi on its MIDI rings (midi.c) and the settings as ALSA controls
 * (controls.c). core.c probes the device.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_H
#define OZZY_H

#include <linux/usb.h>
#include <linux/hrtimer.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/rawmidi.h>

#include "../engine/core/ozzy_engine.h"
#include "../common/midi_running_status.h"

#define OZZY_MAX_INTERFACES  4
#define OZZY_MAX_PIPES        OZZY_PIPE_ROLES
#define OZZY_URBS_PER_PIPE             8     /* per pipe: more than any engine keeps in flight (4) */

#define chip_log(chip, fmt, ...)  dev_info(&(chip)->dev->dev, "[ozzy] " fmt "\n", ##__VA_ARGS__)
#define chip_err(chip, fmt, ...)  dev_err(&(chip)->dev->dev, "[ozzy] " fmt "\n", ##__VA_ARGS__)

struct ozzy_chip;

/* One URB of a pipe and what its completion needs */
struct ozzy_urb {
	struct urb *urb;
	struct ozzy_chip *chip;
	uint8_t pipe;                      /* index in ozzy_urb_bus.pipes */
	uint8_t index;                     /* bit in the pipe's busy mask */
	void *xfer;                        /* the engine's transfer cookie */
	struct ozzy_isoc_packet *packets;  /* isochronous: the engine's packets */
};

struct ozzy_urb_pipe {
	uint8_t address;                   /* bEndpointAddress */
	uint8_t type;                      /* USB_ENDPOINT_XFER_xxx */
	uint8_t interval;                  /* bInterval */
	unsigned int usb_pipe;
	struct ozzy_urb urbs[OZZY_URBS_PER_PIPE];
	unsigned long busy;                /* bit per URB in flight */
};

/*
 * struct ozzy_urb_bus - The engine's bus (engine/core/ozzy_bus.h) on URBs. Completions run in
 * interrupt context; the engine resubmits from there with GFP_ATOMIC.
 * @in_completion: completions running now (ozzy_urb_wait_completions)
 */
struct ozzy_urb_bus {
	struct ozzy_bus bus;
	struct ozzy_chip *chip;
	struct usb_device *dev;
	struct ozzy_urb_pipe pipes[OZZY_MAX_PIPES];
	unsigned int num_pipes;
	struct usb_anchor anchor;          /* every URB in flight */
	atomic_t in_completion;
	struct hrtimer timer;
	ktime_t timer_interval;
	bool timer_on;
	u8 *ctrl_buf;                      /* control requests: the engine passes stack buffers */
};

/* One PCM direction: ALSA's buffer as the engine's ring */
struct ozzy_pcm_stream {
	struct snd_pcm_substream *substream;
	struct ozzy_pcm_ring ring;
	spinlock_t lock;
	bool attached;
	uint64_t pos;                      /* ring position the engine reported last */
	uint64_t period_next;              /* ring position of the next period boundary */
};

struct ozzy_midi {
	struct snd_rawmidi *rmidi;
	struct snd_rawmidi_substream *in;  /* triggered input, NULL if none */
	struct snd_rawmidi_substream *out; /* triggered output, NULL if none */
	spinlock_t lock;
	struct midi_running_status rs;     /* the output stream's running status (PR #80) */
	u8 pending[2];                     /* expanded bytes that didn't fit the ring yet */
	unsigned int pending_count;
};

/*
 * struct ozzy_chip - One device (snd_card private data).
 * @lock: serializes restarts (rate changes, settings) with each other and disconnect
 */
struct ozzy_chip {
	struct usb_device *dev;
	struct snd_card *card;
	struct usb_interface *intf[OZZY_MAX_INTERFACES];   /* [0] probed, the rest claimed */
	unsigned int num_intf;
	const struct ozzy_device *model;
	struct ozzy_engine *engine;
	struct ozzy_shared_memory *shm;    /* CPU only (vzalloc): the transfers have their own memory */
	struct ozzy_urb_bus bus;
	struct mutex lock;
	bool running;                      /* engine started, device present */
	bool gone;                         /* disconnected */
	struct snd_pcm *pcm;
	struct ozzy_pcm_stream stream[OZZY_PCM_DIRS];
	struct ozzy_midi midi;
	struct snd_kcontrol *clock_lock_kctl;
};

/* bus.c */
int ozzy_urb_init(struct ozzy_urb_bus *bus, struct ozzy_chip *chip);
int ozzy_urb_add_pipe(struct ozzy_urb_bus *bus, const struct usb_endpoint_descriptor *epd);
void ozzy_urb_kill(struct ozzy_urb_bus *bus);
void ozzy_urb_wait_completions(struct ozzy_urb_bus *bus);
void ozzy_urb_free(struct ozzy_urb_bus *bus);

/* core.c */
int ozzy_chip_restart(struct ozzy_chip *chip);

/* pcm.c */
int ozzy_pcm_init(struct ozzy_chip *chip);
void ozzy_pcm_progress(struct ozzy_chip *chip, enum ozzy_pcm_dir dir, uint64_t frames);
void ozzy_pcm_stop_streams(struct ozzy_chip *chip);

/* midi.c */
int ozzy_midi_init(struct ozzy_chip *chip);
void ozzy_midi_service(struct ozzy_chip *chip);

/* controls.c */
int ozzy_controls_init(struct ozzy_chip *chip);

#endif /* OZZY_H */
