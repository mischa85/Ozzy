#!/bin/bash

# System tools first: on a case-insensitive disk, libwww-perl's HEAD (/usr/local/bin) runs as head
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

# ==========================================
#  Ozzy Debug Logger
#  "The Black Box Logging Inspector"
# ==========================================

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

TIMESTAMP=$(date "+%Y-%m-%d_%H-%M-%S")
LOGFILE="$HOME/Desktop/Ozzy_Debug_Log_$TIMESTAMP.txt"

PREDICATE='(process == "kernel" AND (composedMessage CONTAINS "[OzzyKext]" OR composedMessage CONTAINS "[Ploytec]")) OR subsystem == "OzzyHAL" OR subsystem == "OzzyMIDI"'

clear
echo "====================================================="
echo "      Ozzy Driver Debugger (History Dump)"
echo "====================================================="
echo ""
echo "This tool collects logs from the *PAST* 2 hours."
echo "Use this after you have reproduced a bug or crash."
echo ""
echo "📝 Writing logs to: $LOGFILE"
echo ""

echo "⏳ Reading system log database (last 2h)..."
echo "   (This may take a few seconds)"

echo "-----------------------------------------------------" >> "$LOGFILE"
echo "OZZY DEBUG LOG - $(date)" >> "$LOGFILE"

# What runs here, so a report doesn't depend on what gets pasted
{
    echo "macOS: $(sw_vers -productVersion) ($(sw_vers -buildVersion)), $(uname -m)"
    echo "SIP: $(csrutil status 2>/dev/null | head -n 1)"
    echo "Loaded kext:"
    { kmutil showloaded 2>/dev/null || kextstat 2>/dev/null; } | grep -i "ozzy" || echo "   (none)"
    echo "Installed kext UUID: $(dwarfdump --uuid /Library/Extensions/OzzyKext.kext/Contents/MacOS/OzzyKext 2>/dev/null | awk '{ print $2; exit }')"
    echo "USB devices:"
    ioreg -p IOUSB -w0 | sed 's/^/   /'
    echo "-----------------------------------------------------"
    INSTALL_LOG=$(ls -t "$HOME"/Desktop/Ozzy_Install_Log_*.txt 2>/dev/null | head -n 1)
    if [ -n "$INSTALL_LOG" ]; then
        echo "LATEST INSTALL LOG ($INSTALL_LOG)"
        cat "$INSTALL_LOG"
    else
        echo "No install log on the Desktop"
    fi
    echo "-----------------------------------------------------"
} >> "$LOGFILE" 2>&1

echo "HISTORY (Start - Last 2h)" >> "$LOGFILE"

# log show came with 10.12; 10.11 writes the kernel and plugin lines to system.log
if command -v log > /dev/null 2>&1; then
    log show --style compact --info --debug --last 2h --no-signpost --predicate "$PREDICATE" >> "$LOGFILE"
else
    grep -E "\[(Ozzy|OzzyKext|Ploytec|UsbMidi|OzzyHAL|OzzyMIDI)\]" /var/log/system.log >> "$LOGFILE"
fi

echo "HISTORY (End)" >> "$LOGFILE"
echo "-----------------------------------------------------" >> "$LOGFILE"

echo ""
echo "✅ Done."
echo "📄 Log saved to Desktop: $LOGFILE"
echo ""
read -p "Press [Enter] to exit..."
