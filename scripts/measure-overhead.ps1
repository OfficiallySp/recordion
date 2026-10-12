<#
Measures what Recordion costs a game: average fps, 1% lows and present mode with Recordion off,
then on, using Intel PresentMon. Run from an elevated terminal (PresentMon needs ETW access):

    powershell -ExecutionPolicy Bypass -File scripts\measure-overhead.ps1 -Process Haste.exe

Stand in the same spot (or replay the same benchmark scene) for both runs.
#>
param(
    [Parameter(Mandatory)] [string] $Process,
    [int] $Seconds = 60
)

$ErrorActionPreference = 'Stop'
$presentMon = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Intel.PresentMon.Console*\presentmon.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $presentMon) { throw "PresentMon not found. Install it with: winget install Intel.PresentMon.Console" }

function Measure-Game([string] $label) {
    $csv = Join-Path $env:TEMP "recordion-presentmon-$label.csv"
    Remove-Item $csv -ErrorAction SilentlyContinue
    Write-Host "  measuring for $Seconds s..."
    & $presentMon.FullName --process_name $Process --timed $Seconds --terminate_after_timed --no_console_stats `
        --v1_metrics --stop_existing_session --output_file $csv | Out-Null
    if (-not (Test-Path $csv)) { throw "PresentMon wrote no data. Is $Process running, and is this terminal elevated?" }
    $rows = Import-Csv $csv
    $frameMs = $rows | ForEach-Object { [double]$_.MsBetweenPresents } | Where-Object { $_ -gt 0 } | Sort-Object
    if ($frameMs.Count -lt 10) { throw "Too few frames captured." }
    $avg = ($frameMs | Measure-Object -Average).Average
    $p99 = $frameMs[[math]::Min($frameMs.Count - 1, [int]($frameMs.Count * 0.99))]
    $modes = $rows | Group-Object PresentMode | Sort-Object Count -Descending |
        ForEach-Object { "{0} ({1:P0})" -f $_.Name, ($_.Count / $rows.Count) }
    [pscustomobject]@{
        Run         = $label
        AvgFps      = [math]::Round(1000 / $avg, 1)
        OnePctLow   = [math]::Round(1000 / $p99, 1)
        PresentMode = $modes -join ', '
    }
}

Write-Host "Step 1 of 2: quit Recordion (tray icon > Exit). Get into the game at a repeatable spot."
Read-Host "Press Enter to measure without Recordion"
$off = Measure-Game 'off'

Write-Host "`nStep 2 of 2: start Recordion and wait for the '<game> - instant replay on' toast. Same spot as before."
Read-Host "Press Enter to measure with Recordion"
$on = Measure-Game 'on'

$off, $on | Format-Table -AutoSize
$delta = ($on.AvgFps - $off.AvgFps) / $off.AvgFps * 100
$lowDelta = ($on.OnePctLow - $off.OnePctLow) / $off.OnePctLow * 100
Write-Host ("Average fps change: {0:+0.0;-0.0}%   1% low change: {1:+0.0;-0.0}%" -f $delta, $lowDelta)
Write-Host "Target: a loss of 3% or less, and the same present mode in both runs."
