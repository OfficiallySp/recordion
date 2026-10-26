<#
Measures what Recordion costs a game: average fps, 1% lows and present mode with Recordion off,
then on, using Intel PresentMon. Alongside, it samples each GPU engine's utilisation for the game,
Recordion and the compositor (dwm), and the APU/GPU power draw where Windows exposes it, so a
"GPU 100%" in Task Manager can be traced to the engine and the process behind it. Task Manager's
headline GPU figure is its busiest engine: the 3D engine for a game, but the Video Encode (on AMD,
"Video Codec") engine is separate hardware that doesn't slow the game down.

Run from an elevated terminal (PresentMon needs ETW access):

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
$gameName = [IO.Path]::GetFileNameWithoutExtension($Process)

# Per-process GPU engine utilisation and power, sampled every 2 s until $until.
function Sample-Gpu([datetime] $until) {
    $game = @(Get-Process -Name $gameName -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $recordion = @(Get-Process -Name recordion -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $dwm = @(Get-Process -Name dwm -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $paths = @('\GPU Engine(*)\Utilization Percentage')
    $hasPower = [bool](Get-Counter -ListSet 'Energy Meter' -ErrorAction SilentlyContinue)
    if ($hasPower) { $paths += '\Energy Meter(apu power)\Power', '\Energy Meter(gpu power)\Power' }
    $byProcess = @{}; $byEngine = @{}; $power = @{}; $n = 0
    while ((Get-Date) -lt $until) {
        $samples = (Get-Counter $paths -SampleInterval 2 -MaxSamples 1 -ErrorAction SilentlyContinue).CounterSamples
        if (-not $samples) { continue }
        $n++
        foreach ($c in $samples) {
            if ($c.Path -like '*energy meter*') {
                $power[$c.InstanceName] = [double]$power[$c.InstanceName] + $c.CookedValue / 1000
                continue
            }
            if ($c.InstanceName -notmatch '^pid_(\d+)_.*_eng_(\d+)_engtype_(.+)$') { continue }
            $procId = [int]$Matches[1]; $engine = $Matches[3]
            $who = if ($game -contains $procId) { 'game' } elseif ($recordion -contains $procId) { 'recordion' }
                   elseif ($dwm -contains $procId) { 'dwm' } else { 'other' }
            $key = "$who|$engine"
            $byProcess[$key] = [double]$byProcess[$key] + $c.CookedValue
            $engineKey = "$($Matches[2])|$engine" # one physical engine, all processes (what Task Manager graphs)
            $byEngine[$engineKey] = [double]$byEngine[$engineKey] + $c.CookedValue
        }
    }
    $n = [math]::Max(1, $n)
    $rows = foreach ($k in $byProcess.Keys) {
        $who, $engine = $k -split '\|', 2
        $pct = $byProcess[$k] / $n
        if ($pct -ge 0.5 -and $who -ne 'other') { [pscustomobject]@{ Process = $who; Engine = $engine; Percent = [math]::Round($pct, 1) } }
    }
    $busiest = foreach ($g in ($byEngine.Keys | Group-Object { ($_ -split '\|', 2)[1] })) {
        $max = ($g.Group | ForEach-Object { $byEngine[$_] / $n } | Measure-Object -Maximum).Maximum
        if ($max -ge 0.5) { "{0} {1:N0}%" -f $g.Name, $max }
    }
    [pscustomobject]@{
        Rows    = $rows | Sort-Object Process, Engine
        Busiest = $busiest -join ', '
        Power   = if ($hasPower) { "APU {0:N1} W, GPU {1:N1} W" -f ($power['apu power'] / $n), ($power['gpu power'] / $n) } else { 'not available' }
    }
}

function Measure-Game([string] $label) {
    $csv = Join-Path $env:TEMP "recordion-presentmon-$label.csv"
    Remove-Item $csv -ErrorAction SilentlyContinue
    Write-Host "  measuring for $Seconds s..."
    $pm = Start-Process -FilePath $presentMon.FullName -PassThru -WindowStyle Hidden -ArgumentList @(
        '--process_name', $Process, '--timed', $Seconds, '--terminate_after_timed', '--no_console_stats',
        '--v1_metrics', '--stop_existing_session', '--output_file', "`"$csv`"")
    $gpu = Sample-Gpu (Get-Date).AddSeconds($Seconds - 2)
    $pm.WaitForExit()
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
        Gpu         = $gpu
    }
}

function Show-Gpu($run) {
    Write-Host ("`n{0}: busiest engines (all processes): {1}; power: {2}" -f $run.Run, $run.Gpu.Busiest, $run.Gpu.Power)
    $run.Gpu.Rows | Format-Table -AutoSize | Out-String | Write-Host
}

Write-Host "Step 1 of 2: quit Recordion (tray icon > Exit). Get into the game at a repeatable spot."
Read-Host "Press Enter to measure without Recordion"
$off = Measure-Game 'off'

Write-Host "`nStep 2 of 2: start Recordion and wait for the '<game> - instant replay on' toast. Same spot as before."
Read-Host "Press Enter to measure with Recordion"
$on = Measure-Game 'on'

$off, $on | Select-Object Run, AvgFps, OnePctLow, PresentMode | Format-Table -AutoSize
$delta = ($on.AvgFps - $off.AvgFps) / $off.AvgFps * 100
$lowDelta = ($on.OnePctLow - $off.OnePctLow) / $off.OnePctLow * 100
Write-Host ("Average fps change: {0:+0.0;-0.0}%   1% low change: {1:+0.0;-0.0}%" -f $delta, $lowDelta)
Write-Host "Target: a loss of 3% or less, and the same present mode in both runs."
Write-Host "`nGPU engine utilisation, averaged over each run (per process; only 0.5% and up):"
Show-Gpu $off
Show-Gpu $on
