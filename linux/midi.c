// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - rawmidi on the engine's MIDI rings
 *
 * The engine sends MIDI out from the shared memory's midi_out ring (embedded in every PCM
 * packet on the Xones, on its own endpoint elsewhere) and fills the midi_in ring. This side
 * moves bytes between those rings and rawmidi: ozzy_midi_service() runs after every completion
 * and timer tick, so the rings keep up with the device. Output gets its running status
 * expanded on the way in (common/midi_running_status.h).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "ozzy.h"

static uint32_t ring_free(volatile struct ozzy_midi_buffer *ring)
{
	return (ozzy_load_acquire_u32(&ring->read_index) - ring->write_index - 1) & OZZY_MIDI_RING_MASK;
}

/* Rawmidi output into midi_out while it has room; expanded bytes that don't fit wait */
static bool fill_out(struct ozzy_chip *chip)
{
	struct ozzy_midi *m = &chip->midi;
	volatile struct ozzy_midi_buffer *ring = &chip->shm->midi_out;
	bool queued = false;
	u8 byte;

	for (;;) {
		if (m->pending_count) {
			if (ring_free(ring) < m->pending_count)
				break;
			ozzy_midi_ring_push(ring, m->pending, m->pending_count);
			m->pending_count = 0;
			queued = true;
		}
		if (!m->out || snd_rawmidi_transmit(m->out, &byte, 1) != 1)
			break;
		m->pending_count = midi_running_status_expand(&m->rs, byte, m->pending);
	}
	return queued;
}

/* midi_in into rawmidi input (dropped while nobody listens) */
static void drain_in(struct ozzy_chip *chip)
{
	struct ozzy_midi *m = &chip->midi;
	u8 bytes[64];
	uint32_t n;

	while ((n = ozzy_midi_ring_pop(&chip->shm->midi_in, bytes, sizeof(bytes))) > 0)
		if (m->in)
			snd_rawmidi_receive(m->in, bytes, n);
}

/* Any context: catch the rings up with rawmidi */
void ozzy_midi_service(struct ozzy_chip *chip)
{
	struct ozzy_midi *m = &chip->midi;
	unsigned long flags;
	bool queued;

	if (!m->rmidi)
		return;
	spin_lock_irqsave(&m->lock, flags);
	drain_in(chip);
	queued = fill_out(chip);
	spin_unlock_irqrestore(&m->lock, flags);
	if (queued)
		ozzy_engine_midi_out_pending(chip->engine);
}

static int ozzy_midi_open(struct snd_rawmidi_substream *substream)
{
	return 0;
}

static int ozzy_midi_close(struct snd_rawmidi_substream *substream)
{
	return 0;
}

static void ozzy_midi_in_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct ozzy_chip *chip = substream->rmidi->private_data;
	unsigned long flags;

	spin_lock_irqsave(&chip->midi.lock, flags);
	chip->midi.in = up ? substream : NULL;
	spin_unlock_irqrestore(&chip->midi.lock, flags);
}

/* While triggered, ozzy_midi_service keeps pulling from the substream */
static void ozzy_midi_out_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct ozzy_chip *chip = substream->rmidi->private_data;
	unsigned long flags;

	spin_lock_irqsave(&chip->midi.lock, flags);
	if (up)
		chip->midi.out = substream;
	else if (chip->midi.out == substream)
		chip->midi.out = NULL;
	spin_unlock_irqrestore(&chip->midi.lock, flags);
	if (up)
		ozzy_midi_service(chip);
}

static const struct snd_rawmidi_ops ozzy_midi_out_ops = {
	.open    = ozzy_midi_open,
	.close   = ozzy_midi_close,
	.trigger = ozzy_midi_out_trigger,
};

static const struct snd_rawmidi_ops ozzy_midi_in_ops = {
	.open    = ozzy_midi_open,
	.close   = ozzy_midi_close,
	.trigger = ozzy_midi_in_trigger,
};

int ozzy_midi_init(struct ozzy_chip *chip)
{
	const struct ozzy_device_profile *p = &chip->engine->profile;
	struct snd_rawmidi *rmidi;
	int ret;

	/* MIDI endpoints, or MIDI in every PCM packet (the Xones) */
	if (!p->pipes[OZZY_PIPE_MIDI_IN].address && !p->pipes[OZZY_PIPE_MIDI_IN].types &&
	    !p->pipes[OZZY_PIPE_MIDI_OUT].address && !p->pipes[OZZY_PIPE_MIDI_OUT].types &&
	    chip->model->family != OZZY_FAMILY_PLOYTEC_XONE)
		return 0;

	ret = snd_rawmidi_new(chip->card, chip->card->shortname, 0, 1, 1, &rmidi);
	if (ret < 0)
		return ret;
	rmidi->private_data = chip;
	strscpy(rmidi->name, chip->dev->product ?: chip->model->name, sizeof(rmidi->name));
	rmidi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_DUPLEX;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &ozzy_midi_out_ops);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &ozzy_midi_in_ops);
	chip->midi.rmidi = rmidi;
	return 0;
}
