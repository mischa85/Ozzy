#!/bin/bash
set -e

# System tools first: on a case-insensitive disk, libwww-perl's HEAD (/usr/local/bin) runs as head
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

clear

# Check for existing installations
KEXT_INSTALLED=false
DEXT_INSTALLED=false
DAEMON_INSTALLED=false
LEGACY_HAL=false

# Check for Kext-based installation
if [ -d "/Library/Extensions/OzzyKext.kext" ] || [ -d "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver" ]; then
    KEXT_INSTALLED=true
fi

# Check for legacy HAL + Daemon (old install-mac.command style)
if [ -d "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver" ] || [ -f "/usr/local/bin/Ozzy" ] || [ -f "/Library/LaunchDaemons/Ozzy.plist" ]; then
    LEGACY_HAL=true
fi

# Check for DriverKit installation
if [ -d "/Applications/Ploytec Driver Extension.app" ]; then
    DEXT_INSTALLED=true
fi

# Check for Userspace Daemon installation (future)
if [ -f "/usr/local/bin/OzzyDaemon" ]; then
    DAEMON_INSTALLED=true
fi

# Show conflict warning if any installation exists
if $KEXT_INSTALLED || $DEXT_INSTALLED || $DAEMON_INSTALLED || $LEGACY_HAL; then
    echo "======================================================"
    echo "          ⚠️  EXISTING INSTALLATION DETECTED"
    echo "======================================================"
    echo ""
    echo "Found installed components:"
    if $KEXT_INSTALLED; then
        echo "  ✓ Legacy Kext Driver"
    fi
    if $LEGACY_HAL; then
        echo "  ✓ Legacy HAL + Daemon (install-mac.command style)"
    fi
    if $DEXT_INSTALLED; then
        echo "  ✓ DriverKit Extension (/Applications/Ploytec Driver Extension.app)"
    fi
    if $DAEMON_INSTALLED; then
        echo "  ✓ Userspace Daemon"
    fi
    echo ""
    echo "⚠️  WARNING: Installing multiple driver types will cause"
    echo "   conflicts and prevent your audio device from working!"
    echo ""
    echo "You must uninstall the existing driver first using:"
    echo "  ./uninstall_ozzy.command"
    echo ""
    echo "Or manually remove:"
    if $KEXT_INSTALLED; then
        echo "  - /Library/Extensions/OzzyKext.kext"
        echo "  - /Library/LaunchDaemons/com.ozzy.kext.load.plist"
    fi
    if $LEGACY_HAL; then
        echo "  - /Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
        echo "  - /Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
        echo "  - /usr/local/bin/Ozzy"
        echo "  - /Library/LaunchDaemons/Ozzy.plist"
    fi
    if $DEXT_INSTALLED; then
        echo "  - /Applications/Ploytec Driver Extension.app"
    fi
    if $DAEMON_INSTALLED; then
        echo "  - /usr/local/bin/OzzyDaemon"
    fi
    echo ""
    echo "Then reboot before installing."
    echo ""
    read -p "Press [Enter] to exit..."
    exit 1
fi

echo "======================================================"
echo "          Ozzy Audio Engine - Full Install"
echo "======================================================"
echo ""
echo "Choose installation type:"
echo ""
echo "  1) Legacy Kext Driver (macOS 10.11 or later)"
echo "     • Kernel Extension (requires SIP modification)"
echo "     • HAL Audio Driver"
echo "     • MIDI Driver"
echo "     • Auto-loads at boot via LaunchDaemon"
echo ""
echo "  2) Modern DriverKit Extension (macOS 11+) [Future]"
echo "     • User-space driver (more secure)"
echo "     • No SIP modification needed"
echo "     • System Extension approval required"
echo "     • NOT YET IMPLEMENTED"
echo ""
echo "  3) Userspace Daemon Driver (All macOS) [Future]"
echo "     • Pure userspace implementation"
echo "     • Shared memory model"
echo "     • No kernel extensions required"
echo "     • NOT YET IMPLEMENTED"
echo ""
read -p "Select option (1, 2, or 3): " -n 1 -r INSTALL_CHOICE
echo
echo ""

if [[ $INSTALL_CHOICE == "2" ]]; then
    echo "⚠️  DriverKit installation not yet implemented."
    echo "   This option will install the DriverKit extension in the future."
    echo ""
    read -p "Press [Enter] to exit..."
    exit 0
elif [[ $INSTALL_CHOICE == "3" ]]; then
    echo "⚠️  Userspace Daemon installation not yet implemented."
    echo "   This option will install the userspace daemon driver"
    echo "   using the shared memory model in the future."
    echo ""
    read -p "Press [Enter] to exit..."
    exit 0
elif [[ $INSTALL_CHOICE != "1" ]]; then
    echo "Invalid choice. Exiting."
    exit 1
fi

echo "======================================================"
echo "    Installing: Legacy Kext Driver"
echo "======================================================"
echo ""

echo ""
echo "======================================================"
echo "Step 1/4: Checking Requirements"
echo "======================================================"

# Check SIP status
echo "🔍 Checking System Integrity Protection (SIP) status..."
SIP_OUTPUT=$(csrutil status 2>/dev/null)

if echo "$SIP_OUTPUT" | grep -q "Kext Signing: disabled"; then
    echo "✅ Kext Signing is disabled - good to proceed"
elif echo "$SIP_OUTPUT" | grep -q "disabled"; then
    echo "✅ SIP is fully disabled - kext loading allowed"
else
    echo "⚠️  WARNING: Kext signing appears to be ENABLED"
    echo ""
    echo "   Modern macOS requires SIP to allow third-party kexts."
    echo "   Recommended: Enable SIP with kext exception (more secure):"
    echo "   1. Reboot into Recovery Mode (hold Cmd+R during startup)"
    echo "   2. Open Terminal from Utilities menu"
    echo "   3. Run: csrutil enable --without kext"
    echo "   4. Reboot normally"
    echo "   5. Run this script again"
    echo ""
    read -p "Continue anyway? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Detect signing identity
echo "🔍 Finding Apple Developer Identity..."
IDENTITY_LINE="$(security find-identity -v -p codesigning | grep "Apple Development" | head -n 1)"
# Xcode before 11 names the free certificate "Mac Developer"
if [ -z "$IDENTITY_LINE" ]; then
    IDENTITY_LINE="$(security find-identity -v -p codesigning | grep "Mac Developer" | head -n 1)"
fi
if [ -z "$IDENTITY_LINE" ]; then
    IDENTITY_LINE="$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -n 1)"
fi

if [ -n "$IDENTITY_LINE" ]; then
    CODE_SIGN_IDENTITY=$(echo "$IDENTITY_LINE" | awk '{print $2}')
    echo "✅ Signing with: $(echo "$IDENTITY_LINE" | cut -d '"' -f 2)"
else
    echo "❌ Error: No valid signing identity found."
    echo "   CoreAudio requires signed plugins."
    exit 1
fi

echo ""
echo "======================================================"
echo "Step 2/4: Compiling Components"
echo "======================================================"

# Compile Kext
echo ""
echo "🔨 [1/5] Compiling Kernel Extension..."

# Kext Configuration
KEXT_NAME="OzzyKext"
BUILD_DIR="$DIR/Build"
SOURCES=(
    "Backends/OzzyKext/OzzyKext.cpp"
    "Backends/OzzyKext/KextBus.cpp"
    "../engine/core/ozzy_engine.c"
    "../engine/core/ozzy_isoc_output.c"
    "../engine/devices/ploytec/ploytec_engine.c"
    "../engine/devices/ploytec/ploytec_io.c"
    "../engine/devices/ploytec/ploytec_isoc_engine.c"
    "../engine/devices/usbmidi/usb_midi_engine.c"
    "../common/devices/ploytec/ploytec_handshake.c"
    "../common/devices/ploytec/ploytec_codec.c"
    "../common/usb_midi.c"
)

# Detect system architecture
SYSTEM_ARCH=$(uname -m)
if [ "$SYSTEM_ARCH" == "arm64" ]; then
    KEXT_ARCH="arm64e"
    echo "   🔍 Detected Apple Silicon (ARM) - building for arm64e"
elif [ "$SYSTEM_ARCH" == "x86_64" ]; then
    KEXT_ARCH="x86_64"
    echo "   🔍 Detected Intel (x86_64) - building for x86_64"
else
    echo "❌ Unknown architecture: $SYSTEM_ARCH"
    exit 1
fi

SDKROOT=$(xcrun --sdk macosx --show-sdk-path)

# Commit we build from, logged by the kext and HAL at startup (common/ozzy_version.h): git
# describe only where the checkout is Ozzy's own, not a repository the sources sit in; else the
# commit git archive / a GitHub ZIP wrote into ozzy_version.txt
ROOT="$(cd "$DIR/.." && pwd -P)"
if [ "$(git -C "$ROOT" rev-parse --show-toplevel 2>/dev/null)" = "$ROOT" ]; then
    GIT_VERSION=$(git -C "$ROOT" describe --always --dirty)
else
    GIT_VERSION=$(cat "$ROOT/common/ozzy_version.txt" 2>/dev/null)
    case "$GIT_VERSION" in ''|*Format*) GIT_VERSION=unknown ;; esac
fi
echo "   🏷️  Build: $GIT_VERSION"
KERNEL_HEADERS="$SDKROOT/System/Library/Frameworks/Kernel.framework/Headers"

# Built for the macOS it is installed on (or the SDK's, if older), never below 10.11
# (IOUSBHostFamily); on older targets the SDK switches os_log to its 10.11 calls, which newer
# systems no longer support
MIN_MACOS=$(printf '%s\n%s\n' "$(sw_vers -productVersion)" "$(xcrun --sdk macosx --show-sdk-version)" |
    awk -F. '{ v = $1 * 100 + $2; if (NR == 1 || v < m) { m = v; s = $1 "." $2 } } END { print s }')
case "$MIN_MACOS" in 10.[0-9]|10.10) MIN_MACOS="10.11" ;; esac

# Devices to leave out (keys from common/ozzy_devices.def), e.g. OZZY_DISABLE="ReloopDJ2" ./install.command.
# Applied to the kext and to its generated personalities alike.
DISABLE_FLAGS=""
for KEY in $OZZY_DISABLE; do DISABLE_FLAGS="$DISABLE_FLAGS -DOZZY_DISABLE_$KEY"; done
if [ -n "$DISABLE_FLAGS" ]; then echo "   🚫 Left out:$DISABLE_FLAGS"; fi

CFLAGS="-x c++ -arch $KEXT_ARCH -mmacosx-version-min=$MIN_MACOS -std=c++11 -O2 -DKERNEL -D__KERNEL__ -mkernel \
-fno-builtin -fno-exceptions -fno-rtti -fno-common -fno-use-cxa-atexit \
-Wno-deprecated-declarations -Wno-inconsistent-missing-override \
-nostdinc -I. -I$KERNEL_HEADERS -include Backends/OzzyKext/SdkCompat.h -DOZZY_GIT_VERSION=\"$GIT_VERSION\"$DISABLE_FLAGS"

# The engine and common/ are C
CFLAGS_C="-x c -arch $KEXT_ARCH -mmacosx-version-min=$MIN_MACOS -std=gnu11 -O2 -DKERNEL -D__KERNEL__ -mkernel \
-fno-builtin -fno-common -Wno-deprecated-declarations -Wno-implicit-int-conversion \
-nostdinc -I. -I$KERNEL_HEADERS -include Backends/OzzyKext/SdkCompat.h -DOZZY_GIT_VERSION=\"$GIT_VERSION\"$DISABLE_FLAGS"

LDFLAGS="-arch $KEXT_ARCH -mmacosx-version-min=$MIN_MACOS -Xlinker -kext -nostdlib -lkmod -lcc_kext"

echo "   🧹 Cleaning build directory..."
# Use sudo to remove if it has root permissions from previous install
sudo rm -rf "$BUILD_DIR" 2>/dev/null || rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/$KEXT_NAME.kext/Contents/MacOS"

echo "   🔨 Compiling kext sources..."
OBJECTS=()
for SRC in "${SOURCES[@]}"; do
    FLAT_NAME=$(basename "${SRC%.*}")
    OBJ="$BUILD_DIR/$FLAT_NAME.o"
    
    if [[ "$SRC" == *.c ]]; then FLAGS="$CFLAGS_C"; else FLAGS="$CFLAGS"; fi
    if ! clang++ $FLAGS -c "$SRC" -o "$OBJ" 2> "$BUILD_DIR/compile.log"; then
        grep -v "warning:" "$BUILD_DIR/compile.log"
        echo "❌ Compilation Failed: $SRC"
        exit 1
    fi
    OBJECTS+=("$OBJ")
done

echo "   🔗 Linking kext..."
if ! clang++ $LDFLAGS "${OBJECTS[@]}" -o "$BUILD_DIR/$KEXT_NAME.kext/Contents/MacOS/$KEXT_NAME"; then
    echo "❌ Linking Failed"
    exit 1
fi

echo "   📦 Bundling kext..."
# The matching personalities come from the device list (common/ozzy_devices.def):
# GenPersonalities prints one per device built for macOS, put in place of the marker
GEN="$BUILD_DIR/GenPersonalities"
if ! clang $DISABLE_FLAGS -o "$GEN" Backends/OzzyKext/GenPersonalities.c || ! "$GEN" > "$BUILD_DIR/personalities.xml"; then
    echo "❌ Error: generating the kext personalities failed"
    exit 1
fi
awk -v f="$BUILD_DIR/personalities.xml" '/OZZY_PERSONALITIES/ { while ((getline line < f) > 0) print line; next } { print }' \
    Backends/OzzyKext/Info.plist.in > "$BUILD_DIR/$KEXT_NAME.kext/Contents/Info.plist"
if ! plutil -lint -s "$BUILD_DIR/$KEXT_NAME.kext/Contents/Info.plist"; then
    echo "❌ Error: the generated Info.plist is invalid"
    exit 1
fi
rm -f "$GEN" "$BUILD_DIR/personalities.xml"

echo "   🔑 Signing kext..."
codesign --force --sign - "$BUILD_DIR/$KEXT_NAME.kext" > /dev/null 2>&1

# macOS links a kext only when it loads, from the LaunchDaemon where nobody sees the error. So check
# here that every symbol comes from a library kext of this macOS, and none from the private KPI
# (third-party kexts may not link it). kmutil came with macOS 11; older systems skip the check.
if command -v kmutil > /dev/null 2>&1; then
    echo "   🔍 Checking the kext links against this macOS..."
    KEXT_BIN="$BUILD_DIR/$KEXT_NAME.kext/Contents/MacOS/$KEXT_NAME"
    FOUND="$(kmutil libraries -p "$BUILD_DIR/$KEXT_NAME.kext" --onedef-symbols 2>/dev/null
             kmutil libraries -p "$BUILD_DIR/$KEXT_NAME.kext" --multdef-symbols 2>/dev/null)"
    MISSING="$(nm -u "$KEXT_BIN" | sort -u | comm -23 - <(echo "$FOUND" | awk '$2 == "in" { print $1 }' | sort -u))"
    PRIVATE="$(echo "$FOUND" | awk '$2 == "in" && /com\.apple\.kpi\.private/ { print $1 }' | sort -u)"
    if [ -n "$MISSING" ] || [ -n "$PRIVATE" ]; then
        [ -n "$MISSING" ] && echo "   Not exported by any kext:" && echo "$MISSING" | sed 's/^/      /'
        [ -n "$PRIVATE" ] && echo "   Only in the private KPI:" && echo "$PRIVATE" | sed 's/^/      /'
        echo "❌ Error: the kext would not load on this macOS"
        exit 1
    fi
fi
echo "✅ Kernel Extension compiled and signed"

# Compile HAL
echo ""
echo "🔨 [2/5] Compiling HAL Driver..."
if ! xcodebuild -project "Ozzy.xcodeproj" \
           -scheme "OzzyHAL" \
           -configuration Release \
           -arch "$SYSTEM_ARCH" \
           SYMROOT="$DIR/Build" \
           MACOSX_DEPLOYMENT_TARGET="$MIN_MACOS" \
           GCC_PREPROCESSOR_DEFINITIONS="\$(inherited) OZZY_GIT_VERSION=\\\"$GIT_VERSION\\\"" \
           OTHER_LDFLAGS="-framework IOKit -framework CoreFoundation" \
           build > "$BUILD_DIR/hal.log" 2>&1; then
    grep -E "error:" "$BUILD_DIR/hal.log" || tail -n 20 "$BUILD_DIR/hal.log"
    echo "❌ Error: building the HAL driver failed (full log: $BUILD_DIR/hal.log)"
    exit 1
fi

HAL_PATH="$DIR/Build/Release/OzzyHAL.driver"
# codesign before 10.14 has no hardened runtime option
codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$HAL_PATH" 2>/dev/null || codesign --sign "$CODE_SIGN_IDENTITY" --force "$HAL_PATH"
echo "✅ HAL Driver compiled and signed"

# Compile MIDI
echo ""
echo "🔨 [3/5] Compiling MIDI Driver..."
if ! xcodebuild -project "Ozzy.xcodeproj" \
           -scheme "OzzyMIDI" \
           -configuration Release \
           -arch "$SYSTEM_ARCH" \
           SYMROOT="$DIR/Build" \
           MACOSX_DEPLOYMENT_TARGET="$MIN_MACOS" \
           OTHER_LDFLAGS="-framework IOKit -framework CoreFoundation -framework CoreMIDI" \
           build > "$BUILD_DIR/midi.log" 2>&1; then
    grep -E "error:" "$BUILD_DIR/midi.log" || tail -n 20 "$BUILD_DIR/midi.log"
    echo "❌ Error: building the MIDI driver failed (full log: $BUILD_DIR/midi.log)"
    exit 1
fi

MIDI_PATH="$DIR/Build/Release/OzzyMIDI.plugin"
codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$MIDI_PATH" 2>/dev/null || codesign --sign "$CODE_SIGN_IDENTITY" --force "$MIDI_PATH"
echo "✅ MIDI Driver compiled and signed"

# Compile ozzy-cli (settings from the command line)
echo ""
echo "🔨 [4/5] Compiling ozzy-cli..."
if ! make -s -C "$DIR/../apps/cli" MACOSX_DEPLOYMENT_TARGET="$MIN_MACOS" > /dev/null; then
    echo "❌ Error: building ozzy-cli failed"
    exit 1
fi
echo "✅ ozzy-cli compiled"

# Compile the ozzy app and make it Ozzy.app. Meson builds it (and fetches libui-ng); without a
# meson and ninja of the system's, pinned ones go into a Python environment of the build's own
# (meson 1.11: the last for the Command Line Tools' Python 3.9). Xcode's clang named, so meson
# doesn't add a ccache it finds (a broken Homebrew one fails the build). The drivers don't need
# the app: if it doesn't build, they install without it.
echo ""
echo "🔨 [5/5] Compiling the ozzy app..."
APP_PATH="$BUILD_DIR/Ozzy.app"
build_app() {
    local gui_build="$BUILD_DIR/gui" venv="$BUILD_DIR/meson-venv"
    if ! command -v meson > /dev/null 2>&1 || ! command -v ninja > /dev/null 2>&1; then
        if [ ! -x "$venv/bin/meson" ] || [ ! -x "$venv/bin/ninja" ]; then
            echo "   meson and ninja aren't installed: putting them in $venv"
            python3 -m venv "$venv" && "$venv/bin/pip" install -q "meson==1.11.2" "ninja==1.13.0" || return 1
        fi
        PATH="$venv/bin:$PATH"
    fi
    if [ -f "$gui_build/build.ninja" ]; then
        CC=clang OBJC=clang MACOSX_DEPLOYMENT_TARGET="$MIN_MACOS" meson setup --reconfigure "$gui_build" "$DIR/../apps/gui" || return 1
    else
        CC=clang OBJC=clang MACOSX_DEPLOYMENT_TARGET="$MIN_MACOS" meson setup "$gui_build" "$DIR/../apps/gui" || return 1
    fi
    meson compile -C "$gui_build" || return 1

    rm -rf "$APP_PATH" &&
        mkdir -p "$APP_PATH/Contents/MacOS" "$APP_PATH/Contents/Resources" &&
        cp "$gui_build/ozzy" "$APP_PATH/Contents/MacOS/ozzy" &&
        cp "$DIR/../apps/gui/ozzy.icns" "$APP_PATH/Contents/Resources/ozzy.icns" &&
        sed -e "s/@VERSION@/$GIT_VERSION/g" -e "s/@MIN_MACOS@/$MIN_MACOS/g" "$DIR/../apps/gui/Info.plist.in" \
            > "$APP_PATH/Contents/Info.plist" || return 1
    codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$APP_PATH" 2>/dev/null ||
        codesign --sign "$CODE_SIGN_IDENTITY" --force "$APP_PATH"
}
if build_app > "$BUILD_DIR/app.log" 2>&1; then
    echo "✅ Ozzy.app compiled and signed"
else
    grep -E "error|ERROR" "$BUILD_DIR/app.log" | head -n 5 || tail -n 10 "$BUILD_DIR/app.log"
    echo "⚠️  The ozzy app didn't build (log: $BUILD_DIR/app.log); the drivers install without it"
    APP_PATH=""
fi

echo ""
echo "======================================================"
echo "Step 3/4: Installing Components"
echo "======================================================"
echo "🔑 Administrator privileges required..."
sudo -v
echo ""

# Install Kext
KEXT_PATH="Build/OzzyKext.kext"
INSTALL_PATH="/Library/Extensions/OzzyKext.kext"

# kmutil came with macOS 11; older systems load with kextload and list with kextstat
if command -v kmutil > /dev/null 2>&1; then
    kext_loaded() { kmutil showloaded 2>/dev/null | grep -q "OzzyKext"; }
    KEXT_LOAD=(kmutil load -p)
else
    kext_loaded() { kextstat 2>/dev/null | grep -q "OzzyKext"; }
    KEXT_LOAD=(/sbin/kextload)
fi

# Unload existing kext if loaded
if kext_loaded; then
    echo "🔄 Unloading currently running kext..."
    sudo killall -9 coreaudiod 2>/dev/null || true
    sleep 1
    sudo kextunload -b "com.ozzy.kext.OzzyKext" 2>/dev/null || true
    sleep 1
fi

# Remove old kext installation
if [ -d "$INSTALL_PATH" ]; then
    echo "🗑️  Removing old kext installation..."
    sudo rm -rf "$INSTALL_PATH"
fi

echo "📦 Installing Kernel Extension..."
sudo chown -R root:wheel "$KEXT_PATH"
sudo chmod -R 755 "$KEXT_PATH"
sudo cp -R "$KEXT_PATH" "$INSTALL_PATH"
sudo chown -R root:wheel "$INSTALL_PATH"
sudo chmod -R 755 "$INSTALL_PATH"
sudo touch /Library/Extensions
echo "✅ Kext installed to /Library/Extensions/"

# Install LaunchDaemon for auto-loading
echo "🚀 Setting up auto-load LaunchDaemon..."
PLIST_PATH="/Library/LaunchDaemons/com.ozzy.kext.load.plist"
PLIST_SOURCE="$DIR/Backends/OzzyKext/com.ozzy.kext.load.plist"

if [ ! -f "$PLIST_SOURCE" ]; then
    echo "❌ Error: LaunchDaemon plist not found at $PLIST_SOURCE"
    exit 1
fi

sudo cp "$PLIST_SOURCE" "$PLIST_PATH"
if [ "${KEXT_LOAD[0]}" != "kmutil" ]; then
    sudo /usr/libexec/PlistBuddy -c "Delete :ProgramArguments" -c "Add :ProgramArguments array" \
        -c "Add :ProgramArguments:0 string /sbin/kextload" -c "Add :ProgramArguments:1 string $INSTALL_PATH" "$PLIST_PATH"
fi
sudo chown root:wheel "$PLIST_PATH"
sudo chmod 644 "$PLIST_PATH"
sudo launchctl load "$PLIST_PATH" 2>/dev/null || true
echo "✅ LaunchDaemon installed"

# Install HAL Driver
echo "📦 Installing HAL Driver..."
if [ -d "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver" ]; then
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
fi
sudo cp -R "$HAL_PATH" "/Library/Audio/Plug-Ins/HAL/"
sudo chown -R root:wheel "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
sudo chmod -R 755 "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
echo "✅ HAL Driver installed"

# Install MIDI Driver
echo "📦 Installing MIDI Driver..."
if [ -d "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin" ]; then
    sudo rm -rf "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
fi
sudo cp -R "$MIDI_PATH" "/Library/Audio/MIDI Drivers/"
sudo chown -R root:wheel "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
sudo chmod -R 755 "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
sudo touch "/Library/Audio/MIDI Drivers"

# Install ozzy-cli (and drop ozzyctl, its old name)
sudo mkdir -p /usr/local/bin
sudo install -m 755 "$DIR/../apps/cli/ozzy-cli" /usr/local/bin/ozzy-cli
sudo rm -f /usr/local/bin/ozzyctl
echo "✅ ozzy-cli installed to /usr/local/bin"

# Install Ozzy.app (an open one quits first: it holds the old build)
if [ -n "$APP_PATH" ]; then
    pkill -x ozzy 2>/dev/null || true
    sudo rm -rf "/Applications/Ozzy.app"
    sudo cp -R "$APP_PATH" "/Applications/"
    echo "✅ The ozzy app installed to /Applications/Ozzy.app"
fi
echo "✅ MIDI Driver installed"

echo ""
echo "======================================================"
echo "Step 4/4: Activating Services"
echo "======================================================"

# Restart audio services
echo "🔄 Restarting audio services..."
sudo killall coreaudiod 2>/dev/null || true
sudo killall MIDIServer 2>/dev/null || true

# Verify the kext we just installed is the one running: a loaded older build doesn't count
sleep 2
KEXT_UUID=$(dwarfdump --uuid "$INSTALL_PATH/Contents/MacOS/$KEXT_NAME" 2>/dev/null | awk '{ print $2; exit }')
kext_current() {
    { kmutil showloaded 2>/dev/null || kextstat 2>/dev/null; } | grep "OzzyKext" | grep -qi "${KEXT_UUID:-no-uuid}"
}
KEXT_LOADED=false
KEXT_OLD=false
if kext_current; then
    echo "✅ Kernel Extension $GIT_VERSION is loaded and running!"
    KEXT_LOADED=true
else
    if kext_loaded; then
        echo "⚠️  An older build of the Kernel Extension is still running"
        KEXT_OLD=true
    fi
    echo "⚠️  Attempting to load Kernel Extension..."
    LOAD_LOG=$(mktemp)
    sudo "${KEXT_LOAD[@]}" "$INSTALL_PATH" > "$LOAD_LOG" 2>&1 || true
    sleep 1
    if kext_current; then
        echo "✅ Kernel Extension $GIT_VERSION loaded successfully!"
        KEXT_LOADED=true
    else
        echo "❌ The new Kernel Extension did not load. ${KEXT_LOAD[0]##*/} said:"
        grep -v "^No variant specified" "$LOAD_LOG" | sed 's/^/      /'
    fi
    rm -f "$LOAD_LOG"
fi

echo ""
echo "======================================================"
echo "          ✅ Installation Complete!"
echo "======================================================"
echo ""

if $KEXT_LOADED; then
    echo "All components installed and running:"
    echo "   ✓ Kernel Extension loaded"
    echo "   ✓ HAL Driver active"
    echo "   ✓ MIDI Driver active"
    echo "   ✓ Auto-load configured for boot"
    echo ""
    echo "Your audio device is ready to use!"
    echo "Check Audio MIDI Setup to verify your device."
    [ -n "$APP_PATH" ] && echo "Its settings: Applications > Ozzy (or ozzy-cli in Terminal)."
    echo ""
    read -p "Press [Enter] to exit..."
else
    echo "⚠️  Installation complete, but kext didn't load automatically."
    echo ""
    echo "A reboot is required to activate the kernel extension."
    echo "After reboot, all components will be active:"
    echo "   • Kernel Extension"
    echo "   • HAL Driver"
    echo "   • MIDI Driver"
    echo ""
    echo "If macOS asks, allow the extension in System Settings > Privacy & Security first."
    echo "After the reboot, this should list the kext with UUID $KEXT_UUID:"
    echo "   kmutil showloaded | grep OzzyKext"
    echo "If it doesn't, the reason is in /tmp/ozzy-kext-load.err"
    echo ""
    read -p "Reboot now? (y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "🔄 Rebooting in 5 seconds... (Ctrl+C to cancel)"
        sleep 5
        sudo reboot
    fi
fi

