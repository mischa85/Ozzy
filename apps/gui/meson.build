# ozzy: the settings app (ozzy_gui.c) on the shared control core and this platform's backend
# (../control/), with libui-ng linked in statically. Meson fetches libui-ng itself at setup, at
# the commit in subprojects/libui-ng.wrap, and applies the patch next to it.
#
#   meson setup apps/gui/build apps/gui && meson compile -C apps/gui/build     -> apps/gui/build/ozzy
#   meson setup apps/gui/build-demo apps/gui -Ddemo=true                       made-up devices, no driver
#
# Linux needs GTK 3 and alsa-lib (Debian/Ubuntu: libgtk-3-dev libasound2-dev); Windows builds
# from a VS developer prompt.
project('ozzy', 'c',
	meson_version: '>=0.63.0',
	license: 'MIT',
	default_options: [
		'buildtype=release',
		'c_std=c11',
		'warning_level=2',
	])

libui = subproject('libui-ng',
	default_options: [
		'default_library=static',
		'buildtype=release',
		'tests=false',
		'examples=false',
	])

os = host_machine.system()
cc = meson.get_compiler('c')

sources = files('ozzy_gui.c', 'ozzy_icon.c', '../control/ozzy_control_core.c')
deps = [libui.get_variable('libui_dep'), libui.get_variable('libui_binary_deps')]

if get_option('demo')
	sources += files('../control/backend_demo.c')
elif os == 'darwin'
	sources += files('../control/backend_macos.c')
	deps += dependency('appleframeworks', modules: ['CoreAudio', 'CoreFoundation'])
elif os == 'windows'
	sources += files('../control/backend_windows.c')
	deps += [cc.find_library('setupapi'), cc.find_library('cfgmgr32')]
else
	sources += files('../control/backend_linux.c')
	deps += dependency('alsa')
endif
if os != 'windows'
	deps += dependency('threads')
endif
# The window's icon (ozzy_icon.c): macOS sets the application's in Objective-C, Linux through GTK
if os == 'darwin'
	add_languages('objc', native: false)
	sources += files('ozzy_icon_macos.m')
	deps += dependency('appleframeworks', modules: ['Cocoa'])
elif os != 'windows'
	deps += dependency('gtk+-3.0')
endif

c_args = []
link_args = []
if os == 'linux' and not get_option('demo')
	c_args += ['-D_GNU_SOURCE']   # realpath, alloca in backend_linux.c (the CLI builds it as gnu11)
endif
if os == 'windows'
	c_args += ['-DUNICODE', '-D_UNICODE', '-D_CRT_SECURE_NO_WARNINGS', '-D_UI_STATIC']
	# libui-ng linked in statically: the app carries the Common Controls 6 manifest itself
	sources += import('windows').compile_resources('ozzy.rc', depend_files: ['ozzy.manifest', 'ozzy.ico'])
	if cc.get_id() == 'msvc'
		link_args += ['/ENTRY:mainCRTStartup']   # main() in a windowed app
	endif
endif

executable('ozzy', sources,
	dependencies: deps,
	c_args: c_args,
	link_args: link_args,
	win_subsystem: 'windows',
	install: true)

# Linux: the launcher entry and the icon GNOME's dock shows (ozzy_icon.c), with meson install.
# Exec is the full path: GLib ignores an entry whose program isn't on the session's PATH, which
# ~/.local/bin often isn't.
if os == 'linux'
	configure_file(input: 'ozzy.desktop.in', output: 'ozzy.desktop',
		configuration: {'BINDIR': get_option('prefix') / get_option('bindir')},
		install_dir: get_option('datadir') / 'applications')
	install_data('ozzy.png', install_dir: get_option('datadir') / 'icons' / 'hicolor' / '256x256' / 'apps')
	# GNOME Shell and GTK only look at an icon theme again when its directory changes (a new file
	# in 256x256/apps doesn't change it), and an icon-theme.cache from before hides the new icon:
	# touch the theme, rebuild a cache that's there. Not into a DESTDIR (packages do their own).
	meson.add_install_script('sh', '-c', '''
		[ -n "$DESTDIR" ] && exit 0
		theme="$MESON_INSTALL_PREFIX/@0@/icons/hicolor"
		touch "$theme"
		[ -f "$theme/icon-theme.cache" ] && gtk-update-icon-cache -qtf "$theme"
		exit 0
	'''.format(get_option('datadir')))
endif
