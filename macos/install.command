#!/bin/bash
set -e

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
echo "  1) Legacy Kext Driver (macOS 10.15 - 15.x)"
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
echo "🔨 [1/3] Compiling Kernel Extension..."

# Kext Configuration
KEXT_NAME="OzzyKext"
BUILD_DIR="$DIR/Build"
SOURCES=(
    "Backends/OzzyKext/OzzyKext.cpp"
    "Backends/OzzyKext/KextBus.cpp"
    "OzzyCore/OzzyEngine.cpp"
    "Devices/Ploytec/PloytecEngine.cpp"
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
KERNEL_HEADERS="$SDKROOT/System/Library/Frameworks/Kernel.framework/Headers"

CFLAGS="-x c++ -arch $KEXT_ARCH -std=c++11 -O2 -DKERNEL -D__KERNEL__ -mkernel \
-fno-builtin -fno-exceptions -fno-rtti -fno-common -fno-use-cxa-atexit \
-Wno-deprecated-declarations -Wno-inconsistent-missing-override \
-nostdinc -I. -I$KERNEL_HEADERS"

LDFLAGS="-arch $KEXT_ARCH -Xlinker -kext -nostdlib -lkmod -lcc_kext"

echo "   🧹 Cleaning build directory..."
# Use sudo to remove if it has root permissions from previous install
sudo rm -rf "$BUILD_DIR" 2>/dev/null || rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/$KEXT_NAME.kext/Contents/MacOS"

echo "   🔨 Compiling kext sources..."
OBJECTS=""
for SRC in "${SOURCES[@]}"; do
    FLAT_NAME=$(basename "${SRC%.*}")
    OBJ="$BUILD_DIR/$FLAT_NAME.o"
    
    clang++ $CFLAGS -c "$SRC" -o "$OBJ" 2>&1 | grep -v "warning:" || true
    
    if [ $? -ne 0 ]; then
        echo "❌ Compilation Failed: $SRC"
        exit 1
    fi
    OBJECTS="$OBJECTS $OBJ"
done

echo "   🔗 Linking kext..."
clang++ $LDFLAGS $OBJECTS -o "$BUILD_DIR/$KEXT_NAME.kext/Contents/MacOS/$KEXT_NAME"

if [ $? -ne 0 ]; then
    echo "❌ Linking Failed"
    exit 1
fi

echo "   📦 Bundling kext..."
if [ -f "Backends/OzzyKext/Info.plist" ]; then
    cp "Backends/OzzyKext/Info.plist" "$BUILD_DIR/$KEXT_NAME.kext/Contents/Info.plist"
else
    echo "❌ Error: Backends/OzzyKext/Info.plist not found!"
    exit 1
fi

echo "   🔑 Signing kext..."
codesign --force --sign - "$BUILD_DIR/$KEXT_NAME.kext" > /dev/null 2>&1
echo "✅ Kernel Extension compiled and signed"

# Compile HAL
echo ""
echo "🔨 [2/3] Compiling HAL Driver..."
xcodebuild -project "Ozzy.xcodeproj" \
           -scheme "OzzyHAL" \
           -configuration Release \
           -arch "$SYSTEM_ARCH" \
           SYMROOT="$DIR/Build" \
           OTHER_LDFLAGS="-framework IOKit -framework CoreFoundation" \
           build > /dev/null

HAL_PATH="$DIR/Build/Release/OzzyHAL.driver"
codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$HAL_PATH"
echo "✅ HAL Driver compiled and signed"

# Compile MIDI
echo ""
echo "🔨 [3/3] Compiling MIDI Driver..."
xcodebuild -project "Ozzy.xcodeproj" \
           -scheme "OzzyMIDI" \
           -configuration Release \
           -arch "$SYSTEM_ARCH" \
           SYMROOT="$DIR/Build" \
           OTHER_LDFLAGS="-framework IOKit -framework CoreFoundation -framework CoreMIDI" \
           build > /dev/null

MIDI_PATH="$DIR/Build/Release/OzzyMIDI.plugin"
codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$MIDI_PATH"
echo "✅ MIDI Driver compiled and signed"

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

# Unload existing kext if loaded
if kmutil showloaded 2>/dev/null | grep -q "OzzyKext"; then
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
echo "✅ MIDI Driver installed"

echo ""
echo "======================================================"
echo "Step 4/4: Activating Services"
echo "======================================================"

# Restart audio services
echo "🔄 Restarting audio services..."
sudo killall coreaudiod 2>/dev/null || true
sudo killall MIDIServer 2>/dev/null || true

# Verify kext is loaded
sleep 2
KEXT_LOADED=false
if kmutil showloaded 2>/dev/null | grep -q "OzzyKext"; then
    echo "✅ Kernel Extension is loaded and running!"
    KEXT_LOADED=true
else
    echo "⚠️  Attempting to load Kernel Extension..."
    sudo kmutil load -p "$INSTALL_PATH"
    sleep 1
    if kmutil showloaded 2>/dev/null | grep -q "OzzyKext"; then
        echo "✅ Kernel Extension loaded successfully!"
        KEXT_LOADED=true
    else
        echo "❌ Failed to load kext. Check logs: cat /tmp/ozzy-kext-load.err"
        KEXT_LOADED=false
    fi
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
    read -p "Reboot now? (y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "🔄 Rebooting in 5 seconds... (Ctrl+C to cancel)"
        sleep 5
        sudo reboot
    fi
fi

