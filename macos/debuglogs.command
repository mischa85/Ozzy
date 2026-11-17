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

# The kext's lines by their prefix and by the kext's binary (what a macOS keeps of an IOLog line
# differs between versions), the plugins' and the daemon's by subsystem
PREDICATE='(process == "kernel" AND (composedMessage CONTAINS "[OzzyKext]" OR composedMessage CONTAINS "[Ploytec]" OR composedMessage CONTAINS "[UsbMidi]" OR senderImagePath CONTAINS "OzzyKext")) OR subsystem == "OzzyHAL" OR subsystem == "OzzyMIDI" OR subsystem == "OzzyDaemon"'

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

# What happened, in the reporter's words, so it travels with the log
echo "Before the logs: what happened? What did you do right before it went wrong,"
echo "and did the Mac crash or restart? (one line, Enter when done)"
read -r -p "> " REPORT_NOTE
echo ""

echo "-----------------------------------------------------" >> "$LOGFILE"
echo "OZZY DEBUG LOG - $(date)" >> "$LOGFILE"
echo "What happened (reporter): ${REPORT_NOTE:-(nothing entered)}" >> "$LOGFILE"

# What runs here, so a report doesn't depend on what gets pasted
{
    echo "macOS: $(sw_vers -productVersion) ($(sw_vers -buildVersion)), $(uname -m)"
    echo "SIP: $(csrutil status 2>/dev/null | head -n 1)"
    echo "Loaded kext:"
    { kmutil showloaded 2>/dev/null || kextstat 2>/dev/null; } | grep -i "ozzy" || echo "   (none)"
    echo "Userspace daemon: $(launchctl print system/com.ozzy.daemon 2>/dev/null | grep -E "^\s+(state|pid) =" | head -n 2 | tr -s ' \t' ' ' | tr '\n' ';' || true)"
    [ -f /usr/local/bin/OzzyDaemon ] || echo "   (not installed)"
    echo "Installed kext UUID: $(dwarfdump --uuid /Library/Extensions/OzzyKext.kext/Contents/MacOS/OzzyKext 2>/dev/null | awk '{ print $2; exit }')"
    echo "USB devices:"
    ioreg -p IOUSB -w0 | sed 's/^/   /'
    echo "OzzyKext in the I/O Registry (whether the kext attached to a device):"
    OZZY_IOREG=$(ioreg -p IOService -w0 -r -c OzzyKext -l 2>/dev/null)
    if [ -n "$OZZY_IOREG" ]; then echo "$OZZY_IOREG" | sed 's/^/   /'; else echo "   (none)"; fi
    echo "-----------------------------------------------------"
    # A kernel panic in the last 2 days: 10.14 and older name them Kernel_*.panic, 11 and later
    # panic-full-*.panic (JSON with a large stackshot, so only the start, which holds the panic
    # string and the backtrace)
    PANIC=$(find /Library/Logs/DiagnosticReports -maxdepth 1 -name "*.panic" ! -name ".*" -mtime -2 2>/dev/null |
        xargs ls -t 2>/dev/null | head -n 1)
    if [ -n "$PANIC" ]; then
        echo "LATEST KERNEL PANIC ($PANIC)"
        head -c 20000 "$PANIC" 2>/dev/null || echo "   (not readable)"
        echo ""
    else
        echo "No kernel panic in the last 2 days"
    fi
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
    grep -E "\[(Ozzy|OzzyKext|OzzyDaemon|Ploytec|UsbMidi|OzzyHAL|OzzyMIDI)\]" /var/log/system.log >> "$LOGFILE"
fi

echo "HISTORY (End)" >> "$LOGFILE"
echo "-----------------------------------------------------" >> "$LOGFILE"

echo ""
echo "✅ Done."
echo "📄 Log saved to Desktop: $LOGFILE"
echo ""
read -p "Press [Enter] to exit..."
