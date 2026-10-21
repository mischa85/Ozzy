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
