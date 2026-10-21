# SPDX-License-Identifier: MIT
#
# Ozzy for Windows: remove the driver from every device and from the driver store, and the ozzy
# app and ozzy-cli. The devices fall back to whatever other driver Windows has for them (or none)
# on the next plug-in.
#
#   uninstall.cmd           the driver, the app and ozzy-cli
#   uninstall.cmd -All      also the certificate it was signed with, and test signing off again
#
# Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>

param(
    [switch]$All,
    [switch]$NoPrompt
)

$ErrorActionPreference = "Stop"

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Run it as administrator (uninstall.cmd asks for that itself)." -ForegroundColor Red
    exit 1
}

# Every Ozzy package in the driver store (its original name is ozzy.inf), with its signer
$packages = @()
$text = pnputil /enum-drivers | Out-String
foreach ($block in $text -split "(?=Published Name:)") {
    if ($block -match "Original Name:\s+ozzy\.inf" -and $block -match "Published Name:\s+(oem\d+\.inf)") {
        $packages += $Matches[1]
    }
}
$signers = @()
foreach ($p in $packages) {
    $cat = Get-ChildItem "$env:windir\System32\DriverStore\FileRepository\ozzy.inf_*\ozzy.cat" -ErrorAction SilentlyContinue
    $signers += $cat | ForEach-Object { (Get-AuthenticodeSignature $_.FullName).SignerCertificate } | Where-Object { $_ }
}

if (-not $packages) {
    Write-Host "Ozzy isn't installed."
} else {
    foreach ($p in $packages) {
        pnputil /delete-driver $p /uninstall /force | Out-Null
        Write-Host "Removed $p"
    }
}

$dest = Join-Path $env:ProgramFiles "Ozzy"
Remove-Item (Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Ozzy.lnk") -ErrorAction SilentlyContinue
if (Test-Path $dest) {
    Get-Process ozzy -ErrorAction SilentlyContinue | Where-Object Path -eq (Join-Path $dest "ozzy.exe") |
        ForEach-Object { $_.Kill(); $_.WaitForExit(5000) | Out-Null }   # an open app holds its file
    Remove-Item -Recurse -Force $dest
    $path = [Environment]::GetEnvironmentVariable("Path", "Machine")
    [Environment]::SetEnvironmentVariable("Path", (($path -split ";") | Where-Object { $_ -and $_ -ne $dest }) -join ";", "Machine")
    Write-Host "Removed the ozzy app and ozzy-cli"
}

if ($All) {
    foreach ($c in $signers | Sort-Object Thumbprint -Unique) {
        foreach ($store in "Root", "TrustedPublisher") {
            Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Thumbprint -eq $c.Thumbprint | Remove-Item
        }
        Write-Host "Certificate removed: $($c.Subject)"
    }
    bcdedit /set testsigning off | Out-Null
    Write-Host "Test signing: off (after a restart)"
}

Write-Host ""
Write-Host "Done. Unplug and replug your device so Windows picks its driver again." -ForegroundColor Green
if (-not $NoPrompt) { Read-Host "Press Enter to close" | Out-Null }
