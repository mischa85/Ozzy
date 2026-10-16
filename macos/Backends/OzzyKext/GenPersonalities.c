/* SPDX-License-Identifier: MIT */
/*
 * Prints the kext's IOKitPersonalities entries, one per device in
 * common/ozzy_devices.def (macOS has an engine for every family). install.command
 * compiles and runs this on the host, with the same -DOZZY_DISABLE_<key> flags as
 * the kext, and puts its output into Info.plist (from Info.plist.in), so the kext
 * matches exactly the devices the code knows.
 *
 *   clang -o gen GenPersonalities.c && ./gen
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */
#include <stdio.h>
#include "../../../common/ozzy_devices.h"

int main(void)
{
	unsigned int i;

	for (i = 0; i < OZZY_NUM_DEVICES; i++) {
		const struct ozzy_device *d = &ozzy_devices[i];

		printf("\t\t<key>%s</key>\n"
		       "\t\t<dict>\n"
		       "\t\t\t<key>CFBundleIdentifier</key>\n"
		       "\t\t\t<string>com.ozzy.kext.OzzyKext</string>\n"
		       "\t\t\t<key>IOClass</key>\n"
		       "\t\t\t<string>OzzyKext</string>\n"
		       "\t\t\t<key>IOProviderClass</key>\n"
		       "\t\t\t<string>IOUSBHostDevice</string>\n"
		       "\t\t\t<key>idVendor</key>\n"
		       "\t\t\t<integer>%u</integer>\n"
		       "\t\t\t<key>idProduct</key>\n"
		       "\t\t\t<integer>%u</integer>\n"
		       "\t\t</dict>\n",
		       d->key, d->vid, d->pid);
	}
	return 0;
}
