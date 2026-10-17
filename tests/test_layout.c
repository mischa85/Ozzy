/* SPDX-License-Identifier: MIT */
/*
 * The shared memory layout that the macOS kext and HAL share (engine/shared/ozzy_shared_data.h):
 * the size and every field's offset. Compared with golden/layout.txt; a change must bump
 * OZZY_SHARED_VERSION. Built as the engine side sees it (OZZY_ENGINE_SIDE).
 */

#include "engine/shared/ozzy_shared_data.h"
#include <stddef.h>
#include <stdio.h>

#define F(member) printf("%-34s %zu\n", #member, offsetof(struct ozzy_shared_memory, member))

int main(void)
{
	printf("version %d size %zu\n", OZZY_SHARED_VERSION, sizeof(struct ozzy_shared_memory));
	F(magic); F(version); F(session_id); F(heartbeat); F(vendor_id); F(product_id);
	F(manufacturer_name); F(product_name); F(serial_number);
	F(midi_out); F(midi_out.write_index); F(midi_out.read_index); F(midi_out.buffer);
	F(midi_in); F(midi_in.write_index); F(midi_in.read_index); F(midi_in.buffer);
	F(midi_in_usb_buffer); F(midi_out_usb_buffer); F(isoc_in_usb_buffer);
	F(audio); F(audio.hardware_present); F(audio.driver_ready); F(audio.sample_rate); F(audio.device_flags);
	F(audio.frames_per_packet); F(audio.samples_per_packet); F(audio.output_bytes_per_frame);
	F(audio.input_bytes_per_frame); F(audio.input_channels); F(audio.output_channels);
	F(audio.safety_offset_frames); F(audio.settings); F(audio.settings.value); F(audio.settings.clock_locked);
	F(audio.settings.lock_rate); F(audio.timestamp.sequence); F(audio.timestamp.sample_time);
	F(audio.timestamp.host_time); F(audio.hal_write_position); F(audio.input_buffer); F(audio.output_buffer);
	return 0;
}
