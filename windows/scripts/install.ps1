# SPDX-License-Identifier: MIT
#
# Ozzy for Windows: check the PC, build the ACX driver, ozzy-cli and the ozzy app (build.ps1),
# install the driver test-signed and bind every connected supported device to it.
#
#   install.cmd                          check, build, install
#   install.cmd -NoBuild                 install the package already built (windows\build\x64\Release\ozzy\)
#   install.cmd -Package C:\path\ozzy    install a package from elsewhere (ozzy.inf, ozzy.sys, ozzy.cat)
#   install.cmd -Disable TascamUS144,NumarkNS7
#                                        build without those devices (keys from common\ozzy_devices.def),
#                                        so another driver keeps them
#
# Ozzy is test-signed, not signed by Microsoft: Windows loads it only with Secure Boot off and test
# signing on. The first install turns test signing on and asks for a restart.
#
# Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>

param(
    [switch]$NoBuild,
    [string]$Package,
    [string]$Disable,
    [switch]$NoPrompt
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$windows = Split-Path -Parent $here
$built = Join-Path $windows "build\x64\Release"
if ($Package) { $NoBuild = $true } else { $Package = Join-Path $built "ozzy" }
$restart = $null   # why Windows needs a restart, if it does

function Say($text) { Write-Host $text }
function Stop-Install($text) {
    Write-Host ""
    Write-Host "Not installed: $text" -ForegroundColor Red
    if (-not $NoPrompt) { Read-Host "Press Enter to close" | Out-Null }
    exit 1
}

Say "Ozzy for Windows"
Say ""

# 1. The PC: everything that would stop Ozzy from loading, before anything is built or changed
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Stop-Install "run it as administrator (install.cmd asks for that itself)."
}
$os = Get-CimInstance Win32_OperatingSystem
if ([int]$os.BuildNumber -lt 22000) {
    Stop-Install "Ozzy needs Windows 11 (its audio driver framework, ACX, is new there); this is build $($os.BuildNumber)."
}
if ($env:PROCESSOR_ARCHITECTURE -ne "AMD64") {
    Stop-Install "Ozzy is built for x64 PCs only for now; this one is $($env:PROCESSOR_ARCHITECTURE)."
}
Say "Windows: $($os.Caption) (build $($os.BuildNumber)), x64"

# Secure Boot has to be off: with it on, Windows ignores test signing
$secureBoot = $null
try { $secureBoot = Confirm-SecureBootUEFI } catch { $secureBoot = $null }   # $null: legacy BIOS or no access
if ($secureBoot -eq $true) {
    Stop-Install ("Secure Boot is on. Ozzy is test-signed, which Windows only loads with Secure Boot off.`n" +
                  "Turn Secure Boot off in the PC's firmware settings (UEFI), then run this again.")
}
Say "Secure Boot: off"

$current = (bcdedit /enum "{current}") -join "`n"
$testSigning = $current -match "testsigning\s+Yes"

if (-not $NoBuild) {
    $free = (Get-PSDrive ($env:SystemDrive.TrimEnd(":"))).Free
    if ($free -lt 5GB) { Stop-Install "the build tools need about 5 GB free on $env:SystemDrive ($([int]($free / 1GB)) GB now)." }
}
Say ""

# 2. Build (fetches the build tools and the WDK the first time)
if (-not $NoBuild) {
    try { & (Join-Path $here "build.ps1") -Disable $Disable } catch { Stop-Install $_.Exception.Message }
    Say ""
}

$inf = Join-Path $Package "ozzy.inf"
$cat = Join-Path $Package "ozzy.cat"
if (-not (Test-Path $inf) -or -not (Test-Path $cat) -or -not (Test-Path (Join-Path $Package "ozzy.sys"))) {
    Stop-Install "no driver package in $Package (ozzy.inf, ozzy.sys, ozzy.cat): run install.cmd without -NoBuild."
}

# 3. Test signing
if ($testSigning) {
    Say "Test signing: on"
} else {
    bcdedit /set testsigning on | Out-Null
    if ($LASTEXITCODE -ne 0) { Stop-Install "bcdedit couldn't turn test signing on." }
    Say "Test signing: turned on (takes effect after a restart)"
    $restart = "test signing takes effect then"
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
        if ($needReboot -and -not $restart) { $restart = "Windows asked for it to switch the device" }
    } else {
        Say "  $($dev.FriendlyName) ($id): not switched (error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    }
}
if ($updating) { Start-Service AudioEndpointBuilder, Audiosrv }
if ($bound -eq 0) { Say "No supported device is connected now: it gets Ozzy when you plug it in." }

# 6. The ozzy app and ozzy-cli in Program Files: the app in the Start menu ("Ozzy"), ozzy-cli on the PATH
$dest = Join-Path $env:ProgramFiles "Ozzy"
$builtDir = Split-Path -Parent $Package
New-Item -ItemType Directory -Force $dest | Out-Null
$app = Join-Path $builtDir "ozzy.exe"
if (Test-Path $app) {
    Get-Process ozzy -ErrorAction SilentlyContinue | Where-Object Path -eq (Join-Path $dest "ozzy.exe") |
        ForEach-Object { $_.Kill(); $_.WaitForExit(5000) | Out-Null }   # an open app holds its file
    Copy-Item $app $dest -Force
    $shell = New-Object -ComObject WScript.Shell
    $lnk = $shell.CreateShortcut((Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Ozzy.lnk"))
    $lnk.TargetPath = Join-Path $dest "ozzy.exe"
    $lnk.Description = "Ozzy device settings"
    $lnk.Save()
    Say "The ozzy app: Start menu > Ozzy ($dest\ozzy.exe)"
}
$cliExe = Join-Path $builtDir "ozzy-cli.exe"
if (Test-Path $cliExe) {
    Copy-Item $cliExe $dest -Force
    $path = [Environment]::GetEnvironmentVariable("Path", "Machine")
    if (($path -split ";") -notcontains $dest) {
        [Environment]::SetEnvironmentVariable("Path", "$path;$dest", "Machine")
    }
    Say "ozzy-cli: $dest\ozzy-cli.exe (on the PATH in new windows)"
}

Say ""
if ($restart) {
    Write-Host "Installed. Restart Windows to finish ($restart)." -ForegroundColor Yellow
} else {
    Write-Host "Installed." -ForegroundColor Green
}
if (-not $NoPrompt) { Read-Host "Press Enter to close" | Out-Null }
