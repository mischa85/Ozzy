// SPDX-License-Identifier: MIT
/*
 * macOS: ozzy.png as the application's icon (Dock, app switcher), once NSApp exists (uiInit).
 * Installed, the app is Ozzy.app with ozzy.icns (macos/install.command); this is for the plain
 * executable meson builds, which has no bundle and so no icon of its own. Called from ozzy_icon.c.
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
