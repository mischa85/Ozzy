/* SPDX-License-Identifier: MIT */
/*
 * Prints ozzy.inf: the template (ozzy.inf.in) with its "; OZZY_DEVICES" line replaced by one
 * model line per device in common/ozzy_devices.def that the Windows driver supports (the
 * Ploytec bulk/interrupt family so far), so the driver matches exactly the devices the code
 * knows. ozzy.vcxproj compiles and runs this on the host before the build; the Windows
 * counterpart of GenPersonalities.c.
 *
 *   cl GenInf.c && GenInf ozzy.inf.in > ozzy.inf
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */
#include <stdio.h>
#include <string.h>
#include "../common/ozzy_devices.h"

#define MARKER "; OZZY_DEVICES"

static void print_devices(void)
{
	unsigned int i;

	for (i = 0; i < OZZY_NUM_DEVICES; i++) {
		const struct ozzy_device *d = &ozzy_devices[i];

		if (d->family != OZZY_FAMILY_PLOYTEC_XONE)
			continue;
		printf("\"%s\" = Ozzy.Device, USB\\VID_%04X&PID_%04X\n", d->name, d->vid, d->pid);
	}
}

int main(int argc, char **argv)
{
	char line[1024];
	int markers = 0;
	FILE *f;

	if (argc != 2) {
		fprintf(stderr, "usage: GenInf ozzy.inf.in > ozzy.inf\n");
		return 2;
	}
	f = fopen(argv[1], "r");
	if (!f) {
		fprintf(stderr, "GenInf: can't read %s\n", argv[1]);
		return 1;
	}
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, MARKER, strlen(MARKER))) {
			print_devices();
			markers++;
		} else {
			fputs(line, stdout);
		}
	}
	fclose(f);
	if (markers != 1) {
		fprintf(stderr, "GenInf: %s needs exactly one '%s' line\n", argv[1], MARKER);
		return 1;
	}
	return 0;
}
