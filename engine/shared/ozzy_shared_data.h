/* SPDX-License-Identifier: MIT */
/*
 * The shared memory between an engine and its clients: on macOS the kext maps it into OzzyHAL
 * and OzzyMIDI; on Windows the driver and the harness keep it to themselves. C, usable from
 * C++: the engine side (ozzy_platform.h, OZZY_ENGINE_SIDE) sees plain volatile fields, the
 * client side std::atomic ones, with the same layout.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_SHARED_DATA_H
#define OZZY_SHARED_DATA_H

#include "ozzy_platform.h"
#include "../../common/ozzy_settings.h"

#ifdef OZZY_ENGINE_SIDE
typedef volatile uint32_t ozzy_atomic_u32;
typedef volatile uint64_t ozzy_atomic_u64;
typedef volatile bool     ozzy_atomic_bool;
#else
#include <atomic>
typedef std::atomic<uint32_t> ozzy_atomic_u32;
typedef std::atomic<uint64_t> ozzy_atomic_u64;
typedef std::atomic<bool>     ozzy_atomic_bool;
#endif

#define OZZY_SHARED_MEM_NAME            "/ozzy_shared_mem"
#define OZZY_MAGIC                      0x4F5A5A59   /* "OZZY" */
#define OZZY_SHARED_VERSION             7            /* bump when ozzy_shared_memory changes layout */

/* OzzyKext user client: external method selectors */
#define OZZY_METHOD_MIDI_OUT_PENDING    0            /* OzzyMIDI queued bytes in midi_out */
/*
 * OzzyHAL changes a device setting (common/ozzy_settings.h). Scalars in: setting id, value.
 * The kext stops the transfers, re-runs the handshake with the setting and restarts them, then
 * returns; the result is in audio.settings and audio.sample_rate. The HAL calls it from
 * PerformDeviceConfigurationChange, with IO stopped.
 */
#define OZZY_METHOD_APPLY_SETTING       1
/*
 * OzzyHAL device property for the settings (common/ozzy_settings.h), for ozzyctl or an app. A
 * custom property (declared in kAudioObjectPropertyCustomPropertyInfoList), so its data is a
 * CFPropertyList, no qualifier:
 *   get: CFDictionary, setting name (ozzy_setting_descs[].name) -> CFNumber value, for every
 *        setting the device has, plus "Clock Locked" (CFBoolean) and "Lock Rate" (CFNumber, Hz)
 *        with the clock source; the caller releases it
 *   set: CFDictionary of setting name -> CFNumber value, one or more. Routes apply at once; a
 *        device setting goes through a configuration change, so it reads back changed once
 *        CoreAudio has performed it
 * The device has it if it has any setting.
 */
#define OZZY_PROPERTY_SETTING           'ozst'
#define OZZY_AUDIO_PROPERTY_KEY         "OzzyAudio"  /* registry: true if the device has audio channels */

/* Sizing: 8 KB covers a High Speed USB transfer; devices use what they need */
#define OZZY_MAX_PACKET_SIZE            8192
#define OZZY_NUM_PACKETS                128          /* logical packets in each ring */
#define OZZY_USB_BUFFERS                4            /* USB buffers in flight (ping-pong) */
#define OZZY_PACKET_MASK                (OZZY_USB_BUFFERS - 1)

/*
 * The engine publishes a timestamp anchor every OZZY_TIMESTAMP_PACKETS packets; the HAL reports
 * that spacing as the zero timestamp period. CoreAudio caps the IO buffer size at 3/8 of the
 * period (measured: 8 packets of 80 frames -> 240 frames max, too small for 256+ buffers), so
 * 40 packets (3200 frames) allows buffers up to 1200 frames. Must stay below OZZY_NUM_PACKETS.
 */
#define OZZY_TIMESTAMP_PACKETS          40

/* ozzy_audio_buffer.device_flags */
#define OZZY_DEVICE_FLAG_PLOYTEC_BULK   (1u << 0)    /* Ploytec 512-byte "bulk" output layout, else 482-byte "interrupt" */
#define OZZY_DEVICE_FLAG_PLAIN_OUTPUT   (1u << 1)    /* output_buffer is a ring of plain interleaved frames (see below) */
#define OZZY_DEVICE_FLAG_SPDIF_MIRROR   (1u << 2)    /* Ploytec bulk, 8 out: lanes 9/10 mirror a pair (ESU1808) */

/* Isochronous IN transfers (feedback): 2 in flight, up to 32 packets of up to 256 bytes each */
#define OZZY_ISOC_IN_BUFFER_SIZE        (2 * 32 * 256)

#define OZZY_MIDI_RING_SIZE             1024
#define OZZY_MIDI_RING_MASK             (OZZY_MIDI_RING_SIZE - 1)

/* The device's clock: an anchor every OZZY_TIMESTAMP_PACKETS, as a seqlock (odd sequence while
 * the engine writes) */
struct ozzy_timestamp {
	ozzy_atomic_u32 sequence;
	ozzy_atomic_u64 sample_time;     /* frames */
	ozzy_atomic_u64 host_time;       /* the platform's clock (ozzy_bus_ops.time) */
};

struct ozzy_audio_buffer {
	ozzy_atomic_bool hardware_present;
	ozzy_atomic_bool driver_ready;
	ozzy_atomic_u32 sample_rate;
	ozzy_atomic_u32 device_flags;          /* OZZY_DEVICE_FLAG_* */

	/* Frame format, set by the engine */
	uint32_t frames_per_packet;            /* frames per logical packet (80 for Ploytec) */
	uint32_t samples_per_packet;           /* frames per USB sub-packet (10 for Ploytec) */
	uint32_t output_bytes_per_frame;       /* encoded bytes per output frame (e.g. 48) */
	uint32_t input_bytes_per_frame;        /* encoded bytes per input frame (e.g. 64) */
	uint32_t input_channels;               /* channels the device sends (e.g. 8, or 16 for the WZ4) */
	uint32_t output_channels;              /* channels the device receives (e.g. 8, or 2 for the WZ4) */
	uint32_t safety_offset_frames;         /* how far ahead of the clock the engine reads output (0: 2 packets) */

	/*
	 * Device settings (common/ozzy_settings.h) as the engine applied them at its last
	 * handshake, and what the device reported. Written before driver_ready at start, and before
	 * OZZY_METHOD_APPLY_SETTING returns, which is when the HAL reads them.
	 */
	struct {
		uint32_t available;            /* bit (1 << enum ozzy_setting) per setting the device has */
		uint8_t value[OZZY_NUM_SETTINGS];
		uint8_t clock_locked;          /* locked to S/PDIF (clock source Automatic only) */
		uint32_t lock_rate;            /* Hz while clock_locked */
	} settings;

	struct ozzy_timestamp timestamp;

	uint8_t _pad1[64];
	OZZY_CACHE_ALIGN ozzy_atomic_u64 hal_write_position;
	uint8_t _pad2[64];

	/*
	 * Input: logical packets at OZZY_MAX_PACKET_SIZE stride. Output: logical packets at
	 * OZZY_MAX_PACKET_SIZE stride in the device's packet layout, or with
	 * OZZY_DEVICE_FLAG_PLAIN_OUTPUT a ring of OZZY_NUM_PACKETS * frames_per_packet plain frames
	 * of output_bytes_per_frame bytes (interleaved S24_3LE), which isochronous transfers read
	 * directly.
	 */
	uint8_t input_buffer[OZZY_MAX_PACKET_SIZE * OZZY_NUM_PACKETS];
	uint8_t output_buffer[OZZY_MAX_PACKET_SIZE * OZZY_NUM_PACKETS];
};

/* A single-producer, single-consumer byte ring */
struct ozzy_midi_buffer {
	OZZY_CACHE_ALIGN ozzy_atomic_u32 write_index;
	uint8_t _pad1[64];
	OZZY_CACHE_ALIGN ozzy_atomic_u32 read_index;
	uint8_t _pad2[64];
	uint8_t buffer[OZZY_MIDI_RING_SIZE];
};

struct ozzy_shared_memory {
	uint32_t magic;                        /* OZZY_MAGIC */
	uint32_t version;
	uint32_t session_id;
#ifdef OZZY_ENGINE_SIDE
	ozzy_atomic_u32 heartbeat;
#else
	ozzy_atomic_u32 heartbeat { 0 };
#endif

	/* The device, as the HAL publishes it */
	uint16_t vendor_id;
	uint16_t product_id;
	char manufacturer_name[64];
	char product_name[64];
	char serial_number[64];

	struct ozzy_midi_buffer midi_out;      /* clients fill it, the engine drains it */
	struct ozzy_midi_buffer midi_in;       /* the engine fills it, clients drain it */

	/* MIDI IN transfers (as large as a bulk endpoint's packet) */
	uint8_t midi_in_usb_buffer[512];
	/* MIDI OUT transfers, for devices with a MIDI OUT endpoint */
	uint8_t midi_out_usb_buffer[64];
	/* Isochronous IN transfers (feedback endpoint) */
	uint8_t isoc_in_usb_buffer[OZZY_ISOC_IN_BUFFER_SIZE];

	OZZY_CACHE_ALIGN struct ozzy_audio_buffer audio;
};

#ifdef OZZY_ENGINE_SIDE
/* --- MIDI rings, engine side: it fills midi_in and drains midi_out --- */

/* Append @count bytes to @ring; what doesn't fit is dropped. Returns how many went in. */
static inline uint32_t ozzy_midi_ring_push(volatile struct ozzy_midi_buffer *ring, const uint8_t *bytes,
					   uint32_t count)
{
	uint32_t w = ring->write_index;
	uint32_t r = ozzy_load_acquire_u32(&ring->read_index);
	uint32_t i;

	for (i = 0; i < count && ((w + 1) & OZZY_MIDI_RING_MASK) != r; i++) {
		ring->buffer[w] = bytes[i];
		w = (w + 1) & OZZY_MIDI_RING_MASK;
	}
	ozzy_store_release_u32(&ring->write_index, w);   /* release: the bytes before the index */
	return i;
}

/* Take up to @max bytes from @ring into @dst. Returns how many. */
static inline uint32_t ozzy_midi_ring_pop(volatile struct ozzy_midi_buffer *ring, uint8_t *dst, uint32_t max)
{
	uint32_t r = ring->read_index;
	uint32_t w = ozzy_load_acquire_u32(&ring->write_index);   /* acquire: the bytes before the index */
	uint32_t i;

	for (i = 0; i < max && r != w; i++) {
		dst[i] = ring->buffer[r];
		r = (r + 1) & OZZY_MIDI_RING_MASK;
	}
	ozzy_store_release_u32(&ring->read_index, r);
	return i;
}
#endif

#endif /* OZZY_SHARED_DATA_H */
