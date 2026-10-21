# SPDX-License-Identifier: MIT
#
# Ozzy for Windows: build the ACX driver package, ozzy-cli and the ozzy app, getting the tools
# first if needed (installing them needs administrator rights).
#
#   powershell -ExecutionPolicy Bypass -File build.ps1      (install.cmd runs it for you)
#
# What it needs, and fetches when it isn't there:
#   - the Visual Studio 2026 Build Tools with only the C++ compiler and the WDK's build integration
#     (a few GB; an installed Visual Studio 2026 gets those components added)
#   - the WDK and the Windows SDK as NuGet packages (about 330 MB) in windows\build\wdk\, the
#     versions pinned and checked below. No WDK install and no EWDK (20 GB).
#   - meson (its own installer, with ninja) for the ozzy app, which fetches libui-ng at setup
#
# Output: windows\build\x64\Release\ozzy\ (ozzy.inf, ozzy.sys, ozzy.cat, test-signed),
# windows\build\x64\Release\ozzy-cli.exe and windows\build\x64\Release\ozzy.exe.
#
# Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>

param(
    [string]$GitVersion,   # the commit to stamp the driver with (default: git describe, if git is there)
    [string]$Disable       # devices to leave out, keys from common\ozzy_devices.def: "TascamUS144,NumarkNS7"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is many times slower with its progress bar
$windows = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$repo = Split-Path -Parent $windows
$build = Join-Path $windows "build"
$wdkDir = Join-Path $build "wdk"

# The WDK and the SDK it goes with, from nuget.org (SHA-512 as nuget.org lists it). The WDK's
# build tasks need MSBuild 18: Visual Studio 2026.
$wdkVersion = "10.0.28000.2526"
$packages = [ordered]@{
    "Microsoft.Windows.SDK.CPP"     = "YQFVBXfvKdqjl8V6YduBtDCcu6Vi8v+aLpoc8oqM7d+RO1p8FQnS1ON8u3x9f/g52a0McEFiODtvxqnkLM5ltA=="
    "Microsoft.Windows.SDK.CPP.x64" = "rMIUohVAehjlNuvAHzE50Brgi4zjU2yQ5+7+6D/i0117jtrzqAm2OCr0rnTVZcffAxBUygXF1JLUEK0+opttnA=="
    "Microsoft.Windows.WDK.x64"     = "dAGSbkjTr4lFX6hVncyctBDSg87O7x3QSUr8Yf9dFLZ0T3TWuo+i4WrXdviSW77Bdmzn2iu1pA83wGe1V/1akg=="
}
# MSVC, and the WDK's build integration (the kernel-driver toolset, with the Spectre-mitigated
# libraries it pulls in)
# Meson (with ninja) for the ozzy app, from its own installer (no Python needed)
$mesonVersion = "1.12.1"
$mesonSha256 = "0e541b6ae656cd9c8f9d5f8ac9eddc22c68d37006ae1dfd055e3ab485519804d"
$vsComponents = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "Component.Microsoft.Windows.DriverKit.BuildTools"

function Say($text) { Write-Host $text }
function Fail($text) { throw $text }
# A native tool's output into the log. In Windows PowerShell, a tool writing to stderr is an error
# of its own while $ErrorActionPreference is Stop: here only the exit code counts.
function Invoke-Logged($exe) {
    $ErrorActionPreference = "Continue"
    & $exe @args 2>&1 | Out-File $log -Append -Encoding utf8
}

# 1. MSBuild 18, MSVC and the WDK's build integration
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
function Find-VS {
    if (-not (Test-Path "$installer\vswhere.exe")) { return $null }
    & "$installer\vswhere.exe" -version "[18.0," -products * -requires $vsComponents -latest -property installationPath
}
$vs = Find-VS
if (-not $vs) {
    $existing = if (Test-Path "$installer\vswhere.exe") {
        & "$installer\vswhere.exe" -version "[18.0," -products * -latest -property installationPath }
    $add = $vsComponents | ForEach-Object { "--add"; $_ }
    if ($existing) {
        Say "Adding the C++ and driver build tools to $existing (a few minutes)..."
        $p = Start-Process "$installer\setup.exe" -ArgumentList (@("modify", "--installPath", "`"$existing`"", "--quiet", "--norestart") + $add) -Wait -PassThru
    } else {
        Say "Getting the Visual Studio 2026 Build Tools (C++ compiler and driver build tools only, a few GB; this takes a while)..."
        $boot = Join-Path $env:TEMP "vs_buildtools.exe"
        Invoke-WebRequest "https://aka.ms/vs/18/stable/vs_buildtools.exe" -OutFile $boot
        $sig = Get-AuthenticodeSignature $boot
        if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notmatch "O=Microsoft Corporation") {
            Fail "the Build Tools download isn't signed by Microsoft."
        }
        $p = Start-Process $boot -ArgumentList (@("--quiet", "--wait", "--norestart", "--nocache", "--add", "Microsoft.VisualStudio.Workload.VCTools") + $add) -Wait -PassThru
    }
    # 3010: done, a restart is due (not needed for building)
    if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { Fail "the Visual Studio installer failed (exit code $($p.ExitCode))." }
    $vs = Find-VS
    if (-not $vs) { Fail "the C++ build tools aren't there after installing them." }
}
$msbuild = Join-Path $vs "MSBuild\Current\Bin\amd64\MSBuild.exe"
$vcVersion = (Get-Content (Join-Path $vs "VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt")).Trim()
$vc = Join-Path $vs "VC\Tools\MSVC\$vcVersion"
Say "Build tools: $vs (MSVC $vcVersion)"

# 2. The WDK and SDK packages, unpacked once into windows\build\wdk\<id>\
$stamp = Join-Path $wdkDir "version.txt"
if (-not (Test-Path $stamp) -or (Get-Content $stamp) -ne $wdkVersion) {
    Say "Getting the WDK $wdkVersion from nuget.org (about 330 MB)..."
    Say "  (Microsoft's license for it ends up in $wdkDir\Microsoft.Windows.WDK.x64\LICENSE.txt)"
    if (Test-Path $wdkDir) { Remove-Item -Recurse -Force $wdkDir }
    New-Item -ItemType Directory -Force $wdkDir | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    foreach ($id in $packages.Keys) {
        $lower = $id.ToLower()
        $nupkg = Join-Path $wdkDir "$id.nupkg"
        Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/$lower/$wdkVersion/$lower.$wdkVersion.nupkg" -OutFile $nupkg
        $hash = [Convert]::ToBase64String([Security.Cryptography.SHA512]::Create().ComputeHash([IO.File]::ReadAllBytes($nupkg)))
        if ($hash -ne $packages[$id]) { Fail "$id doesn't match its pinned hash." }
        [IO.Compression.ZipFile]::ExtractToDirectory($nupkg, (Join-Path $wdkDir $id))
        Remove-Item $nupkg
        Say "  $id"
    }
    Set-Content $stamp $wdkVersion
}

# 3. The driver package (ozzy.vcxproj finds the WDK in windows\build\wdk\ itself)
$log = Join-Path $build "build.log"
Set-Content $log "" -Encoding utf8
# git describe only where the checkout is Ozzy's own, not a repository the sources sit in; else
# the commit git archive / a GitHub ZIP wrote into ozzy_version.txt (common/ozzy_version.h)
if (-not $GitVersion -and (Get-Command git -ErrorAction SilentlyContinue)) {
    $top = & { $ErrorActionPreference = "Continue"; git -C $repo rev-parse --show-toplevel 2>$null }
    if ($top -and [IO.Path]::GetFullPath($top).TrimEnd('\') -eq [IO.Path]::GetFullPath($repo).TrimEnd('\')) {
        $GitVersion = & { $ErrorActionPreference = "Continue"; git -C $repo describe --always --dirty 2>$null }
    }
}
if (-not $GitVersion) {
    $versionFile = Join-Path $repo "common\ozzy_version.txt"
    if (Test-Path $versionFile) {
        $v = (Get-Content $versionFile -TotalCount 1).Trim()
        if ($v -and $v -notmatch "Format") { $GitVersion = $v }
    }
}
$props = @("/p:Configuration=Release", "/p:Platform=x64")
if ($GitVersion) { $props += "/p:OzzyGitVersion=$GitVersion" }
if (-not $Disable) { $Disable = $env:OZZY_DISABLE }
if ($Disable) {
    # Spaces between the keys: /p: takes ; and , as the end of the property
    $keys = $Disable -split "[\s,;]+" | Where-Object { $_ }
    $props += "/p:OzzyDisable=$($keys -join ' ')"
    Say "Leaving out: $($keys -join ', ')"
}
Say "Building the driver..."
Invoke-Logged $msbuild (Join-Path $windows "driver\ozzy.vcxproj") /nologo /m /v:minimal @props
if ($LASTEXITCODE -ne 0) {
    Get-Content $log | Select-String "error" | Select-Object -First 15 | ForEach-Object { Write-Host "  $_" }
    Fail "the driver build failed (full log: $log)."
}

# 4. ozzy-cli, with the same compiler and SDK (statically linked: no runtime to install)
Say "Building ozzy-cli..."
$sdk = Join-Path $wdkDir "Microsoft.Windows.SDK.CPP\c\Include\$($wdkVersion -replace '\.\d+$', '.0')"
$sdkLib = Join-Path $wdkDir "Microsoft.Windows.SDK.CPP.x64\c"
$out = Join-Path $build "x64\Release"
$obj = Join-Path $build "obj\x64\Release\ozzy-cli"
New-Item -ItemType Directory -Force $obj | Out-Null
$env:INCLUDE = "$vc\include;$sdk\ucrt;$sdk\um;$sdk\shared"
$env:LIB = "$vc\lib\x64;$sdkLib\ucrt\x64;$sdkLib\um\x64"
$cli = Join-Path $repo "apps\cli"
Invoke-Logged "$vc\bin\Hostx64\x64\cl.exe" /nologo /std:c11 /W3 /O2 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS `
    "/Fo$obj\\" "/Fe$out\ozzy-cli.exe" "$cli\ozzy_cli.c" "$cli\..\control\ozzy_control_core.c" "$cli\..\control\backend_windows.c" `
    setupapi.lib cfgmgr32.lib
if ($LASTEXITCODE -ne 0) {
    Get-Content $log | Select-String "error" | Select-Object -Last 15 | ForEach-Object { Write-Host "  $_" }
    Fail "the ozzy-cli build failed (full log: $log)."
}

# 5. The ozzy app (meson, libui-ng fetched at setup), with the same compiler and SDK. Not
# fatal: the driver and ozzy-cli work without it.
Say "Building the ozzy app..."
$meson = Get-Command meson -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (-not $meson -and (Test-Path "$env:ProgramFiles\Meson\meson.exe")) { $meson = "$env:ProgramFiles\Meson\meson.exe" }
if (-not $meson) {
    Say "  Getting meson $mesonVersion..."
    $msi = Join-Path $env:TEMP "meson-$mesonVersion-64.msi"
    Invoke-WebRequest "https://github.com/mesonbuild/meson/releases/download/$mesonVersion/meson-$mesonVersion-64.msi" -OutFile $msi
    if ((Get-FileHash $msi -Algorithm SHA256).Hash -ne $mesonSha256) { Fail "the meson download doesn't match its pinned hash." }
    $p = Start-Process msiexec.exe -ArgumentList "/i", "`"$msi`"", "/qn", "/norestart" -Wait -PassThru
    Remove-Item $msi
    if ($p.ExitCode -eq 0 -and (Test-Path "$env:ProgramFiles\Meson\meson.exe")) { $meson = "$env:ProgramFiles\Meson\meson.exe" }
}
$gui = Join-Path $build "gui"
$guiOk = $false
if ($meson) {
    # cl and rc on the PATH: meson takes them as they are, no developer prompt
    $env:PATH = "$vc\bin\Hostx64\x64;$(Join-Path $wdkDir "Microsoft.Windows.SDK.CPP\c\bin\$($wdkVersion -replace '\.\d+$', '.0')\x64");$(Split-Path $meson);$env:PATH"
    if (-not (Test-Path (Join-Path $gui "build.ninja"))) {
        Invoke-Logged $meson setup $gui (Join-Path $repo "apps\gui") --buildtype=release -Db_vscrt=mt
    }
    if ($LASTEXITCODE -eq 0) { Invoke-Logged $meson compile -C $gui }
    if ($LASTEXITCODE -eq 0) { Copy-Item (Join-Path $gui "ozzy.exe") $out -Force; $guiOk = $true }
}
if ($guiOk) {
    Say "Built: $out\ozzy\, $out\ozzy-cli.exe and $out\ozzy.exe"
} else {
    Write-Host "The ozzy app didn't build (log: $log); the driver and ozzy-cli did: $out" -ForegroundColor Yellow
}
