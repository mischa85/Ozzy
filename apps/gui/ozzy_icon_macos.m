// SPDX-License-Identifier: MIT
/*
 * macOS: ozzy.png as the application's icon (Dock, app switcher). The app runs as a plain
 * executable, without a bundle and its icon file, so it sets its own once NSApp exists
 * (uiInit). Called from ozzy_icon.c.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#import <Cocoa/Cocoa.h>

void ozzy_icon_set_app(const unsigned char *png, unsigned long size)
{
	NSImage *icon = [[NSImage alloc] initWithData:[NSData dataWithBytes:png length:size]];

	if (icon)
		[NSApp setApplicationIconImage:icon];   /* NSApp keeps it */
	[icon release];
}
