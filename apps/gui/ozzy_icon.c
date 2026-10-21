// SPDX-License-Identifier: MIT
/*
 * The window's icon, Ozzy the cat, which libui-ng has no call for: each platform's own.
 *   - Windows: the exe's icon resource (ozzy.rc, ozzy.ico) on the window, the title bar's and
 *     the taskbar's (WM_SETICON, both sizes)
 *   - Linux: ozzy.png (ozzy_icon_png.h) on the window and as the default for any other (GTK),
 *     which X11 shows. GNOME on Wayland has no window icons: its dock and app switcher show the
 *     icon of the .desktop file named as the window's app id, so the app id is "ozzy" (libui-ng
 *     starts GTK without argv, which leaves it unnamed) and meson install puts ozzy.desktop and
 *     the icon in the hicolor theme
 *   - macOS: windows have no icon of their own there; ozzy.png becomes the application's, in
 *     the Dock and the app switcher (ozzy_icon_macos.m), since the app runs without a bundle
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "ui.h"
#include "ozzy_icon.h"

#if defined(_WIN32)
#include <windows.h>

void ozzy_icon_init(void)
{
}

void ozzy_icon_set(uiWindow *w)
{
	HWND hwnd = (HWND)uiControlHandle(uiControl(w));
	HINSTANCE self = GetModuleHandleW(NULL);
	/* Not "small": rpcndr.h defines it */
	HANDLE large_icon = LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
				       GetSystemMetrics(SM_CYICON), 0);
	HANDLE small_icon = LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
				       GetSystemMetrics(SM_CYSMICON), 0);

	if (large_icon)
		SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)large_icon);
	if (small_icon)
		SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
}

#elif defined(__APPLE__)
#include "ozzy_icon_png.h"

void ozzy_icon_set_app(const unsigned char *png, unsigned long size);   /* ozzy_icon_macos.m */

void ozzy_icon_init(void)
{
}

void ozzy_icon_set(uiWindow *w)
{
	(void)w;
	ozzy_icon_set_app(ozzy_icon_png, sizeof(ozzy_icon_png));
}

#else
#include <gtk/gtk.h>
#include "ozzy_icon_png.h"

/* The app id GDK gives the window on Wayland (and its WM_CLASS on X11): ozzy.desktop's name */
void ozzy_icon_init(void)
{
	g_set_prgname("ozzy");
	g_set_application_name("Ozzy");
}

void ozzy_icon_set(uiWindow *w)
{
	GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
	GdkPixbuf *icon = NULL;

	if (gdk_pixbuf_loader_write(loader, ozzy_icon_png, sizeof(ozzy_icon_png), NULL) &&
	    gdk_pixbuf_loader_close(loader, NULL))
		icon = gdk_pixbuf_loader_get_pixbuf(loader);
	if (icon) {
		gtk_window_set_icon(GTK_WINDOW(uiControlHandle(uiControl(w))), icon);
		gtk_window_set_default_icon(icon);
	}
	g_object_unref(loader);
}
#endif
