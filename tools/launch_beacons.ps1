<#
.SYNOPSIS
    Opens several SoundBeacon windows (fake game clients) for testing Window Alerts.

.EXAMPLE
    .\tools\launch_beacons.ps1 -Count 5            # five windows, press Space in each to cast
    .\tools\launch_beacons.ps1 -Count 5 -Auto      # five windows fishing by themselves
    .\tools\launch_beacons.ps1 -Count 3 -LoudCast  # cast as loud as the splash (tests "Ignore after input")
#>
param(
    [int]$Count = 5,
    [switch]$Auto,
    [switch]$LoudCast,
    [string]$Beacon = "",
    [string]$Log = ""
)

if (-not $Beacon) {
    $candidates = @(
        "$PSScriptRoot\SoundBeacon.exe",
        "$PSScriptRoot\..\build_release\bin\Release\SoundBeacon.exe",
        "$PSScriptRoot\..\build\bin\Release\SoundBeacon.exe",
        "$PSScriptRoot\..\build\bin\SoundBeacon.exe"
    )
    $Beacon = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Beacon -or -not (Test-Path $Beacon)) {
    Write-Error "SoundBeacon.exe not found. Build the project first or pass -Beacon <path>."
    exit 1
}

$castVolume = if ($LoudCast) { "0.3" } else { "0.05" }
for ($i = 1; $i -le $Count; $i++) {
    $args = @("--index=$i", "--tile", "--cast-volume=$castVolume", "--splash-volume=0.3")
    if ($Auto) { $args += "--auto" }
    if ($Log) { $args += "--log=`"$Log`"" }
    Start-Process -FilePath $Beacon -ArgumentList $args | Out-Null
}
Write-Host "Started $Count beacon window(s). Each one is a separate process with its own audio session."
