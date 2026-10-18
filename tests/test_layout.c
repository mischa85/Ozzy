/* SPDX-License-Identifier: MIT */
/*
 * The shared memory layout that the macOS kext and HAL share (engine/shared/ozzy_shared_data.h):
 * the size and every field's offset. Compared with golden/layout.txt; a change must bump
 * OZZY_SHARED_VERSION. Built as the engine side sees it (OZZY_ENGINE_SIDE). Then struct
 * ozzy_info (engine/shared/ozzy_info.h), which every driver hands to the tools as it is: a
 * change must bump OZZY_INFO_VERSION.
 */

#include "engine/shared/ozzy_shared_data.h"
#include <stddef.h>
#include <stdio.h>

#define F(member) printf("%-34s %zu\n", #member, offsetof(struct ozzy_shared_memory, member))
#define I(member) printf("info.%-29s %zu\n", #member, offsetof(struct ozzy_info, member))

int main(void)
{
	printf("version %d size %zu\n", OZZY_SHARED_VERSION, sizeof(struct ozzy_shared_memory));
	F(magic); F(version); F(session_id); F(heartbeat); F(vendor_id); F(product_id);
	F(manufacturer_name); F(product_name); F(serial_number); F(info);
	F(midi_out); F(midi_out.write_index); F(midi_out.read_index); F(midi_out.buffer);
	F(midi_in); F(midi_in.write_index); F(midi_in.read_index); F(midi_in.buffer);
	F(midi_in_usb_buffer); F(midi_out_usb_buffer); F(isoc_in_usb_buffer);
	F(audio); F(audio.hardware_present); F(audio.driver_ready); F(audio.sample_rate); F(audio.device_flags);
	F(audio.output_ring_frames); F(audio.input_ring_frames); F(audio.timestamp_frames); F(audio.output_bytes_per_frame);
	F(audio.input_bytes_per_frame); F(audio.input_channels); F(audio.output_channels);
	F(audio.safety_offset_frames); F(audio.input_safety_offset_frames); F(audio.settings); F(audio.settings.value); F(audio.settings.clock_locked);
	F(audio.settings.lock_rate); F(audio.timestamp.sequence); F(audio.timestamp.sample_time);
	F(audio.timestamp.host_time); F(audio.hal_write_position); F(audio.input_buffer); F(audio.output_buffer);

	printf("info version %d size %zu\n", OZZY_INFO_VERSION, sizeof(struct ozzy_info));
	I(version); I(size); I(starts); I(vendor_id); I(product_id); I(family); I(status); I(usb_speed);
	I(alt_setting); I(interface_count); I(interfaces); I(output); I(feedback); I(firmware_valid);
	I(firmware_chip); I(firmware); I(input_selector_valid); I(input_selector); I(clock_locked);
	I(sample_rate); I(boot_rate); I(lock_rate); I(device_rates); I(host_rates); I(in_channels);
	I(out_channels); I(transfer_frames); I(out_bytes_per_frame);
	I(in_bytes_per_frame); I(safety_offset_frames); I(input_safety_offset_frames); I(timestamp_frames); I(driver_version); I(pipes);
	printf("info.pipes[] size %zu\n", sizeof(struct ozzy_info_pipe));
	return 0;
}
