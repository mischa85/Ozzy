# SPDX-License-Identifier: MIT
# MIDI out on the wire: capture with USBPcap while winmm_midi.py sends to the port named -Port,
# then list the MIDI bytes in the PCM out packets (midi_out_slots.py). They should be the sent
# bytes, in order.
#   powershell -ExecutionPolicy Bypass -File midi_out_check.ps1 [-Port xone] [-Pcap \\.\USBPcap1]
param([string]$Port = "xone", [string]$Pcap = "\\.\USBPcap1",
      [string[]]$Messages = @("903c64", "b0552a", "803c00", "c005"), [string]$Sysex = "f07e7f0601f7")
$f = Join-Path $env:TEMP "midi_out_check.pcap"; Remove-Item $f -ErrorAction SilentlyContinue
$p = Start-Process "C:\Program Files\USBPcap\USBPcapCMD.exe" -ArgumentList '-d', $Pcap, '-A', '-o', $f -PassThru -WindowStyle Hidden
Start-Sleep 2
python "$PSScriptRoot\winmm_midi.py" send $Port @Messages
if ($Sysex) { python "$PSScriptRoot\winmm_midi.py" sysex $Port $Sysex }
Start-Sleep 1
Stop-Process -Id $p.Id -Force; Start-Sleep 1   # USBPcap keeps the file locked until it ends
python "$PSScriptRoot\midi_out_slots.py" $f
