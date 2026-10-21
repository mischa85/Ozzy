# SPDX-License-Identifier: MIT
#
# Ozzy for Windows: install the ACX driver (test-signed) and bind every connected supported device to it.
#
#   install.cmd                          a package already built (x64\Release\ozzy\ next to this script)
#   install.cmd -Build                   build it first with the EWDK (mounted, or found as C:\ewdk\*.iso)
#   install.cmd -Build -Ewdk D:\x.iso    build with that EWDK ISO
#   install.cmd -Package C:\path\ozzy    install a package from elsewhere (ozzy.inf, ozzy.sys, ozzy.cat)
#
# Ozzy is test-signed, not signed by Microsoft: Windows loads it only with Secure Boot off and test
# signing on. The first install turns test signing on and asks for a restart.
#
# Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>

param(
    [switch]$Build,
    [string]$Ewdk,
    [string]$Package,
    [switch]$NoPrompt
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Package) { $Package = Join-Path $here "x64\Release\ozzy" }
$restart = $false

function Say($text) { Write-Host $text }
function Stop-Install($text) {
    Write-Host ""
    Write-Host "Not installed: $text" -ForegroundColor Red
    if (-not $NoPrompt) { Read-Host "Press Enter to close" | Out-Null }
    exit 1
}

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Stop-Install "run it as administrator (install.cmd asks for that itself)."
}

Say "Ozzy for Windows"
Say ""

# 1. Secure Boot has to be off: with it on, Windows ignores test signing
$secureBoot = $null
try { $secureBoot = Confirm-SecureBootUEFI } catch { $secureBoot = $null }   # $null: legacy BIOS or no access
if ($secureBoot -eq $true) {
    Stop-Install ("Secure Boot is on. Ozzy is test-signed, which Windows only loads with Secure Boot off.`n" +
                  "Turn Secure Boot off in the PC's firmware settings (UEFI), then run this again.")
}
Say "Secure Boot: off"

# 2. Test signing
$current = (bcdedit /enum "{current}") -join "`n"
if ($current -match "testsigning\s+Yes") {
    Say "Test signing: on"
} else {
    bcdedit /set testsigning on | Out-Null
    if ($LASTEXITCODE -ne 0) { Stop-Install "bcdedit couldn't turn test signing on." }
    Say "Test signing: turned on (takes effect after a restart)"
    $restart = $true
}

# 3. Build with the EWDK, if asked
if ($Build) {
    $setup = Get-PSDrive -PSProvider FileSystem | ForEach-Object { Join-Path $_.Root "BuildEnv\SetupBuildEnv.cmd" } |
             Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $setup) {
        if (-not $Ewdk) { $Ewdk = Get-ChildItem C:\ewdk\*.iso -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName }
        if (-not $Ewdk) { Stop-Install "no EWDK found: mount the EWDK ISO, or pass -Ewdk <path to the ISO>." }
        $volume = Mount-DiskImage $Ewdk -PassThru | Get-Volume
        $setup = "$($volume.DriveLetter):\BuildEnv\SetupBuildEnv.cmd"
    }
    Say "Building with the EWDK ($setup)..."
    $log = Join-Path $env:TEMP "ozzy-build.log"
    cmd /c "call `"$setup`" amd64 >nul 2>&1 && cd /d `"$here`" && msbuild ozzy.vcxproj /nologo /m /p:Configuration=Release /p:Platform=x64 /v:minimal > `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Get-Content $log | Select-String "error" | Select-Object -First 15 | ForEach-Object { Write-Host "  $_" }
        Stop-Install "the build failed (full log: $log)."
    }
    Say "Built."
}

$inf = Join-Path $Package "ozzy.inf"
$cat = Join-Path $Package "ozzy.cat"
if (-not (Test-Path $inf) -or -not (Test-Path $cat) -or -not (Test-Path (Join-Path $Package "ozzy.sys"))) {
    Stop-Install "no driver package in $Package (ozzy.inf, ozzy.sys, ozzy.cat): build it with -Build."
}

# 4. Trust the certificate the package is signed with, so Windows installs it without asking
$signer = (Get-AuthenticodeSignature $cat).SignerCertificate
if (-not $signer) { Stop-Install "ozzy.cat isn't signed." }
foreach ($store in "Root", "TrustedPublisher") {
    if (-not (Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Thumbprint -eq $signer.Thumbprint)) {
        $s = New-Object Security.Cryptography.X509Certificates.X509Store($store, "LocalMachine")
        $s.Open("ReadWrite"); $s.Add($signer); $s.Close()
    }
}
Say "Certificate trusted: $($signer.Subject)"

# 5. The package into the driver store, then the devices onto it
$out = pnputil /add-driver $inf 2>&1 | Out-String
if ($LASTEXITCODE -ne 0 -and $out -notmatch "already") { Write-Host $out; Stop-Install "pnputil couldn't add the driver package." }
Say "Driver package added."

# Every supported device that is connected goes to Ozzy, also when another driver has it (the
# manufacturer's, WinUSB from Zadig): Windows' own update call with the force flag
Add-Type -Namespace Ozzy -Name NewDev -MemberDefinition @"
[DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hardwareId, string infPath, uint flags, out bool reboot);
"@
$ids = Select-String -Path $inf -Pattern "USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}" -AllMatches | ForEach-Object { $_.Matches.Value } | Sort-Object -Unique
$present = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match "^USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}\\" }
$bound = 0
# Ozzy already running a device (an update): Windows' audio services hold its endpoints open, so the
# old driver couldn't unload before a restart. Stopped for the switch, it can (a few seconds of
# no sound on every device).
$ozzyInfs = (pnputil /enum-drivers | Out-String) -split "(?=Published Name:)" |
            Where-Object { $_ -match "Original Name:\s+ozzy\.inf" -and $_ -match "Published Name:\s+(oem\d+\.inf)" } | ForEach-Object { $Matches[1] }
$updating = $present | Where-Object {
    (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data -in $ozzyInfs }
if ($updating) {
    Say "Updating a running Ozzy: Windows' audio stops for a few seconds."
    Stop-Service AudioEndpointBuilder -Force   # Audiosrv depends on it and stops too
}
foreach ($id in $ids) {
    $dev = $present | Where-Object { $_.InstanceId -like "$id\*" } | Select-Object -First 1
    if (-not $dev) { continue }
    $needReboot = $false
    if ([Ozzy.NewDev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, $id, $inf, 1, [ref]$needReboot)) {
        Say "  $($dev.FriendlyName) ($id): Ozzy"
        $bound++
        if ($needReboot) { $restart = $true }
    } else {
        Say "  $($dev.FriendlyName) ($id): not switched (error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    }
}
if ($updating) { Start-Service AudioEndpointBuilder, Audiosrv }
if ($bound -eq 0) { Say "No supported device is connected now: it gets Ozzy when you plug it in." }

Say ""
if ($restart) {
    Write-Host "Installed. Restart Windows to finish (test signing takes effect then)." -ForegroundColor Yellow
} else {
    Write-Host "Installed." -ForegroundColor Green
}
if (-not $NoPrompt) { Read-Host "Press Enter to close" | Out-Null }
