// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - ALSA PCM on the engine's rings
 *
 * ALSA's buffer is the engine's ring (struct ozzy_pcm_ring): the engine encodes playback from
 * it and decodes capture into it in the USB completion path, and reports each packet
 * (ozzy_pcm_progress), which moves the pointer and elapses periods. trigger attaches and
 * detaches the ring (a store, fine in atomic context); sync_stop waits out completions that
 * may still use it before ALSA frees or reuses the buffer.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <sound/pcm_params.h>

#include "ozzy.h"

static const unsigned int rate_bits[OZZY_MAX_RATES] = {
	SNDRV_PCM_RATE_44100, SNDRV_PCM_RATE_48000, SNDRV_PCM_RATE_88200,
	SNDRV_PCM_RATE_96000, SNDRV_PCM_RATE_176400, SNDRV_PCM_RATE_192000,
};

static enum ozzy_pcm_dir substream_dir(const struct snd_pcm_substream *substream)
{
	return substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? OZZY_PCM_OUT : OZZY_PCM_IN;
}

/* hw_params rule: while the device is locked to S/PDIF, only the lock rate (the official
 * driver offers CoreAudio only the current rate) */
static int ozzy_pcm_rule_locked_rate(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	struct ozzy_chip *chip = rule->private;
	struct snd_interval lock = { .integer = 1 };

	if (!chip->shm->audio.settings.clock_locked)
		return 0;
	lock.min = lock.max = chip->shm->audio.settings.lock_rate;
	return snd_interval_refine(hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE), &lock);
}

static int ozzy_pcm_open(struct snd_pcm_substream *substream)
{
	struct ozzy_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	const struct ozzy_device *model = chip->model;
	enum ozzy_pcm_dir dir = substream_dir(substream);
	unsigned int channels = dir == OZZY_PCM_OUT ? model->out_channels : model->in_channels;
	uint32_t rates[OZZY_MAX_RATES];
	unsigned int n = ozzy_device_rates(model, rates), i, frames, packet_bytes;

	if (!chip->running)
		return -ENODEV;

	runtime->hw.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
			   SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_MMAP_VALID;
	runtime->hw.formats = SNDRV_PCM_FMTBIT_S24_3LE;
	runtime->hw.rates = 0;
	for (i = 0; i < OZZY_MAX_RATES; i++)
		if (model->rates & (1u << i))
			runtime->hw.rates |= rate_bits[i];
	runtime->hw.rate_min = rates[0];
	runtime->hw.rate_max = rates[n - 1];
	runtime->hw.channels_min = runtime->hw.channels_max = channels;

	/* Periods of at least 2 transfers; isochronous out at the highest rate */
	frames = model->family == OZZY_FAMILY_PLOYTEC_ISOC && dir == OZZY_PCM_OUT ?
		 rates[n - 1] * ISOC_TRANSFER_UFRAMES / ISOC_FB_UFRAMES_PER_SEC : chip->shm->audio.frames_per_packet;
	packet_bytes = frames * channels * 3;
	runtime->hw.buffer_bytes_max = 2000 * packet_bytes;
	runtime->hw.period_bytes_min = 2 * packet_bytes;
	runtime->hw.period_bytes_max = 2000 * packet_bytes;
	runtime->hw.periods_min = 2;
	runtime->hw.periods_max = 1024;

	chip->stream[dir].substream = substream;
	return snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE, ozzy_pcm_rule_locked_rate, chip,
				   SNDRV_PCM_HW_PARAM_CHANNELS, -1);
}

static int ozzy_pcm_close(struct snd_pcm_substream *substream)
{
	struct ozzy_chip *chip = snd_pcm_substream_chip(substream);

	chip->stream[substream_dir(substream)].substream = NULL;
	return 0;
}

/* Process context: a new rate restarts the engine (the handshake at the rate) */
static int ozzy_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct ozzy_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ozzy_pcm_stream *st = &chip->stream[substream_dir(substream)];
	unsigned long flags;
	int ret = 0;

	if (runtime->format != SNDRV_PCM_FORMAT_S24_3LE)
		return -EINVAL;

	mutex_lock(&chip->lock);
	if (!chip->running) {
		ret = -ENODEV;
	} else if (chip->shm->audio.sample_rate != runtime->rate) {
		if (chip->shm->audio.settings.clock_locked && runtime->rate != chip->shm->audio.settings.lock_rate) {
			chip_err(chip, "%u Hz while locked to S/PDIF at %u Hz", runtime->rate,
				  (unsigned int)chip->shm->audio.settings.lock_rate);
			ret = -EINVAL;
		} else {
			chip_log(chip, "sample rate %u -> %u Hz", (unsigned int)chip->shm->audio.sample_rate,
				  runtime->rate);
			ozzy_engine_set_rate(chip->engine, runtime->rate);
			ret = ozzy_chip_restart(chip);
		}
	}
	mutex_unlock(&chip->lock);
	if (ret)
		return ret;

	spin_lock_irqsave(&st->lock, flags);
	st->pos = 0;
	st->period_next = runtime->period_size;
	spin_unlock_irqrestore(&st->lock, flags);
	return 0;
}

/* Atomic: attach or detach the ring; the engine picks it up with its next packet */
static int ozzy_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct ozzy_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	enum ozzy_pcm_dir dir = substream_dir(substream);
	struct ozzy_pcm_stream *st = &chip->stream[dir];
	unsigned long flags;

	spin_lock_irqsave(&st->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		/* START after prepare: from frame 0; a release: where the stream paused */
		st->ring.base = runtime->dma_area;
		st->ring.frames = runtime->buffer_size;
		st->ring.channels = runtime->channels;
		st->ring.start = st->pos;
		st->attached = true;
		ozzy_engine_set_pcm_ring(chip->engine, dir, &st->ring);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		st->attached = false;
		ozzy_engine_set_pcm_ring(chip->engine, dir, NULL);
		break;
	default:
		spin_unlock_irqrestore(&st->lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&st->lock, flags);
	return 0;
}

static snd_pcm_uframes_t ozzy_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct ozzy_chip *chip = snd_pcm_substream_chip(substream);
	struct ozzy_pcm_stream *st = &chip->stream[substream_dir(substream)];
	unsigned long flags;
	uint64_t pos;

	if (!chip->running)
		return SNDRV_PCM_POS_XRUN;
	spin_lock_irqsave(&st->lock, flags);
	pos = st->pos;
	spin_unlock_irqrestore(&st->lock, flags);
	return ozzy_mod_u64(pos, substream->runtime->buffer_size);
}

/* Before ALSA frees or reuses the buffer: no completion may still be using it */
static int ozzy_pcm_sync_stop(struct snd_pcm_substream *substream)
{
	struct ozzy_chip *chip = snd_pcm_substream_chip(substream);

	ozzy_urb_wait_completions(&chip->bus);
	return 0;
}

static const struct snd_pcm_ops ozzy_pcm_ops = {
	.open      = ozzy_pcm_open,
	.close     = ozzy_pcm_close,
	.prepare   = ozzy_pcm_prepare,
	.trigger   = ozzy_pcm_trigger,
	.pointer   = ozzy_pcm_pointer,
	.sync_stop = ozzy_pcm_sync_stop,
};

/*
 * ozzy_pcm_progress - The engine moved a packet through @dir's ring (completion path): the new
 * position, and the periods it completed.
 */
void ozzy_pcm_progress(struct ozzy_chip *chip, enum ozzy_pcm_dir dir, uint64_t frames)
{
	struct ozzy_pcm_stream *st = &chip->stream[dir];
	struct snd_pcm_substream *substream;
	bool elapsed = false;
	unsigned long flags;

	spin_lock_irqsave(&st->lock, flags);
	substream = st->substream;
	if (st->attached && substream) {
		snd_pcm_uframes_t period = substream->runtime->period_size;

		st->pos = frames;
		while (period && frames >= st->period_next) {
			st->period_next += period;
			elapsed = true;
		}
	}
	spin_unlock_irqrestore(&st->lock, flags);
	if (elapsed)
		snd_pcm_period_elapsed(substream);
}

/* Process context: stop running streams with an xrun (a restart is coming) */
void ozzy_pcm_stop_streams(struct ozzy_chip *chip)
{
	unsigned int dir;

	for (dir = 0; dir < OZZY_PCM_DIRS; dir++) {
		struct snd_pcm_substream *substream = chip->stream[dir].substream;

		if (substream && snd_pcm_running(substream))
			snd_pcm_stop_xrun(substream);
	}
}

int ozzy_pcm_init(struct ozzy_chip *chip)
{
	const struct ozzy_device *model = chip->model;
	struct snd_pcm *pcm;
	int ret;

	if (!model->out_channels && !model->in_channels)
		return 0;

	ret = snd_pcm_new(chip->card, chip->dev->product ?: model->name, 0, model->out_channels ? 1 : 0,
			  model->in_channels ? 1 : 0, &pcm);
	if (ret < 0)
		return ret;
	pcm->private_data = chip;
	strscpy(pcm->name, chip->dev->product ?: model->name, sizeof(pcm->name));
	if (model->out_channels)
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &ozzy_pcm_ops);
	if (model->in_channels)
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &ozzy_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);
	chip->pcm = pcm;
	return 0;
}
