# Runs the real ambrosia executable hidden, drives it with a script of steps, and samples its
# private bytes from outside every 100 ms. Each scenario puts "memstats <label>" actions at
# known frames; ambrosia prints its memory counters on stderr (unbuffered) for each, and the
# runner reads the process's private bytes the moment the line arrives.
#
#   pwsh tests/soak/exe-soak.ps1 -Exe build/release/bin/Release/ambrosia.exe -Scenario travel
#
# Scenarios:
#   travel  UNATCO Island <-> UNATCO HQ (or -MapA/-MapB), -Iterations round trips,
#           -StayFrames frames on each level before travelling on.
#   idle    -Map for -Minutes game minutes (30 frames per game second), a marker each minute.
#   fire    like idle, holding the fire button with the assault rifle in hand.
#   save    -Iterations quicksaves, then -Iterations quickloads, on -Map.
#   menus   -Iterations times: open and close inventory, goals, conversations, images,
#           skills, augmentations and health screens on -Map.
#   talk    on UNATCO Island, frob Paul Denton and let the conversation play for -Minutes,
#           a marker every 15 game seconds.
# User data (ini files and saves) goes to a fresh directory, never the user's own.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [ValidateSet('travel', 'idle', 'fire', 'save', 'menus', 'talk')][string]$Scenario = 'travel',
    [string]$MapA = '01_NYC_UNATCOIsland',
    [string]$MapB = '01_NYC_UNATCOHQ',
    [string]$Map = '06_HongKong_WanChai_Market',
    [int]$Iterations = 10,
    [int]$StayFrames = 300,
    [int]$Minutes = 10,
    [int]$Width = 640,
    [int]$Height = 480,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
if (-not $OutDir) {
    $OutDir = Join-Path ([System.IO.Path]::GetTempPath()) "ambrosia-soak-$Scenario-$(Get-Date -Format yyyyMMdd-HHmmss)"
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$userData = Join-Path $OutDir 'userdata'
New-Item -ItemType Directory -Force $userData | Out-Null

$actions = [System.Collections.Generic.List[string]]::new()
$labels = [System.Collections.Generic.List[string]]::new()
function Add-Mark([int]$frame, [string]$label) {
    $label = '{0:D3}_{1}' -f $labels.Count, $label
    $actions.Add("${frame}:memstats $label")
    $labels.Add($label)
}

$startMap = $MapA
switch ($Scenario) {
    'travel' {
        # Mark a third of the stay before leaving, so the sample cannot land in the next load.
        $settle = [int]([math]::Ceiling($StayFrames / 3))
        $frame = $StayFrames
        Add-Mark ($frame - $settle) "at_$MapA"
        for ($i = 1; $i -le $Iterations; $i++) {
            foreach ($target in @($MapB, $MapA)) {
                $actions.Add("${frame}:exec open $target")
                $frame += $StayFrames
                Add-Mark ($frame - $settle) "at_$target"
            }
        }
        $frames = $frame + 60
    }
    { $_ -in 'idle', 'fire' } {
        $startMap = $Map
        $warm = 300
        if ($Scenario -eq 'fire') {
            $actions.Add('30:exec loadout')
            # Belt slot 1 holds the assault rifle after loadout; look at the floor.
            $actions.Add('40:press 1')
            $actions.Add('41:release 1')
            $actions.Add('100:mouse 0 300')
            # After a reload the trigger has to be pressed again, so press it every 4 s.
            for ($f = 120; $f -lt $warm + $Minutes * 1800; $f += 120) {
                $actions.Add("${f}:press LeftMouse")
                $actions.Add("$($f + 110):release LeftMouse")
            }
            # Keep the rifle fed: "set" changes every Ammo762mm object.
            for ($f = 600; $f -lt $warm + $Minutes * 1800; $f += 600) {
                $actions.Add("${f}:exec set DeusEx.Ammo762mm AmmoAmount 150")
            }
        }
        Add-Mark $warm 'start'
        for ($m = 1; $m -le $Minutes; $m++) {
            Add-Mark ($warm + $m * 1800) "minute_$m"
        }
        $frames = $warm + $Minutes * 1800 + 60
    }
    'save' {
        $startMap = $Map
        $frame = 300
        Add-Mark ($frame - 40) 'loaded'
        for ($i = 1; $i -le $Iterations; $i++) {
            $actions.Add("${frame}:exec QuickSave")
            $frame += 60
            Add-Mark ($frame - 30) "quicksave_$i"
        }
        for ($i = 1; $i -le $Iterations; $i++) {
            $actions.Add("${frame}:exec open ?loadgame=-1")
            $frame += 120
            Add-Mark ($frame - 40) "quickload_$i"
        }
        $frames = $frame + 60
    }
    'menus' {
        $startMap = $Map
        $screens = 'ShowInventoryWindow', 'ShowGoalsWindow', 'ShowConversationsWindow', 'ShowImagesWindow', 'ShowSkillsWindow', 'ShowAugmentationsWindow', 'ShowHealthWindow'
        $frame = 200
        Add-Mark ($frame - 10) 'start'
        for ($i = 1; $i -le $Iterations; $i++) {
            foreach ($screen in $screens) {
                $actions.Add("${frame}:exec $screen")
                $actions.Add("$($frame + 10):press Escape")
                $actions.Add("$($frame + 11):release Escape")
                $frame += 20
            }
            Add-Mark ($frame - 5) "round_$i"
        }
        $frames = $frame + 60
    }
    'talk' {
        $startMap = '01_NYC_UNATCOIsland'
        $actions.Add('30:frob PaulDenton0')
        Add-Mark 25 'before'
        $steps = $Minutes * 4
        for ($k = 1; $k -le $steps; $k++) {
            Add-Mark (30 + $k * 450) "t_$($k * 15)s"
        }
        $frames = 30 + $steps * 450 + 60
    }
}

# The scenarios place each action on a frame; the script runs them in that order, waiting out the
# frames between (the first after frame 1), and ends the run on frame $frames.
$script = [System.Collections.Generic.List[string]]::new()
$tick = 1
$ordered = $actions | ForEach-Object { $frame, $action = $_ -split ':', 2; [pscustomobject]@{ Frame = [int]$frame; Action = $action } } | Sort-Object Frame -Stable
foreach ($step in $ordered) {
    if ($step.Frame -gt $tick) {
        $script.Add("wait $($step.Frame - $tick)")
        $tick = $step.Frame
    }
    $script.Add($step.Action)
}
$script.Add("wait $([math]::Max(0, $frames - $tick))")
$script.Add('quit')
$scriptFile = Join-Path $OutDir 'steps.txt'
$script | Set-Content $scriptFile

$arguments = [System.Collections.Generic.List[string]]::new()
$arguments.AddRange([string[]]@($startMap, '--hidden', '--script', $scriptFile, '--width', "$Width", '--height', "$Height"))

$info = [System.Diagnostics.ProcessStartInfo]::new($Exe)
foreach ($argument in $arguments) {
    $info.ArgumentList.Add($argument)
}
$info.UseShellExecute = $false
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$info.WorkingDirectory = $OutDir
$info.Environment['XDG_DATA_HOME'] = $userData
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $info
$stdout = [System.Text.StringBuilder]::new()
$shared = @{
    Process = $process
    Stderr  = [System.Text.StringBuilder]::new()
    Marks   = [System.Collections.Concurrent.ConcurrentQueue[object]]::new()
    Clock   = [System.Diagnostics.Stopwatch]::StartNew()
}
$outEvent = Register-ObjectEvent $process OutputDataReceived -MessageData $stdout -Action { if ($null -ne $EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) } }
$errEvent = Register-ObjectEvent $process ErrorDataReceived -MessageData $shared -Action {
    $line = $EventArgs.Data
    if ($null -eq $line) { return }
    $m = $Event.MessageData
    # The game logs its counters as "DevMemory: memstats <label> ...".
    if ($line.StartsWith('DevMemory: ')) { $line = $line.Substring(11) }
    if ($line.StartsWith('memstats ')) {
        $p = $m.Process
        $private = 0
        try { $p.Refresh(); $private = $p.PrivateMemorySize64 } catch {}
        $m.Marks.Enqueue([pscustomobject]@{ Seconds = $m.Clock.Elapsed.TotalSeconds; Private = $private; Line = $line })
    } else {
        [void]$m.Stderr.AppendLine($line)
    }
}
[void]$process.Start()
$process.BeginOutputReadLine()
$process.BeginErrorReadLine()

$samples = [System.Collections.Generic.List[object]]::new()
while (-not $process.HasExited) {
    try {
        $process.Refresh()
        $samples.Add([pscustomobject]@{
                Seconds    = $shared.Clock.Elapsed.TotalSeconds
                PrivateMiB = $process.PrivateMemorySize64 / 1MB
                WorkingMiB = $process.WorkingSet64 / 1MB
            })
    } catch {
    }
    Start-Sleep -Milliseconds 100
}
$process.WaitForExit()
Start-Sleep -Milliseconds 300
Unregister-Event -SourceIdentifier $outEvent.Name
Unregister-Event -SourceIdentifier $errEvent.Name
$stdout.ToString() | Set-Content (Join-Path $OutDir 'stdout.txt')
$shared.Stderr.ToString() | Set-Content (Join-Path $OutDir 'stderr.txt')
$samples | Export-Csv (Join-Path $OutDir 'samples.csv') -NoTypeInformation

# One row per marker: private MiB plus every counter memstats printed, in MiB where bytes.
$rows = foreach ($mark in $shared.Marks.ToArray()) {
    $fields = $mark.Line -split ' '
    $row = [ordered]@{ Label = $fields[1]; Seconds = [math]::Round($mark.Seconds, 2); PrivateMiB = [math]::Round($mark.Private / 1MB, 2) }
    foreach ($field in $fields[2..($fields.Count - 1)]) {
        $pair = $field -split '=', 2
        $value = [double]$pair[1]
        if ($pair[0] -match 'bytes|data|source|decoded') { $value = [math]::Round($value / 1MB, 3) }
        $row[$pair[0]] = $value
    }
    [pscustomobject]$row
}
$rows | Export-Csv (Join-Path $OutDir 'marks.csv') -NoTypeInformation
$rows | Select-Object Label, Seconds, PrivateMiB, table, destroyed, retired_bytes, sound_decoded, meshdraw, meshdraw_dead, destroyed_windows | Format-Table -AutoSize | Out-String -Width 220 | Write-Output

function Show-Spread([string]$name, [double[]]$values, [string]$unit) {
    if ($values.Count -eq 0) { return }
    $mean = ($values | Measure-Object -Average).Average
    $sd = if ($values.Count -gt 1) { [math]::Sqrt((($values | ForEach-Object { ($_ - $mean) * ($_ - $mean) }) | Measure-Object -Sum).Sum / ($values.Count - 1)) } else { 0 }
    $min = ($values | Measure-Object -Minimum).Minimum
    $max = ($values | Measure-Object -Maximum).Maximum
    "SUMMARY,exe-$Scenario,$name,mean={0:F3},sd={1:F3},min={2:F3},max={3:F3},n={4},$unit" -f $mean, $sd, $min, $max, $values.Count
}

# Per-step deltas of each column between consecutive rows from -From on, optionally only
# rows whose label matches.
function Show-Deltas([object[]]$set, [int]$from, [string]$prefix, [string]$unit, [string[]]$columns) {
    foreach ($column in $columns) {
        $values = @()
        for ($k = [math]::Max($from, 1); $k -lt $set.Count; $k++) { $values += [double]$set[$k].$column - [double]$set[$k - 1].$column }
        Show-Spread "${prefix}_$column" $values $unit
    }
}

$columns = 'PrivateMiB', 'table', 'destroyed', 'data', 'destroyed_data', 'linker_bytes', 'retired_bytes', 'sound_decoded', 'meshdraw', 'meshdraw_dead', 'meshdraw_bytes', 'pose_bytes', 'light_bytes', 'queries', 'destroyed_windows', 'destroyed_window_data'
"exit code $($process.ExitCode), $(@($rows).Count) of $($labels.Count) markers, peak private $([math]::Round(($samples | Measure-Object PrivateMiB -Maximum).Maximum, 1)) MiB"
switch ($Scenario) {
    'travel' {
        # Rows 0-2 are the first visits; a round trip is two rows apart.
        $atA = @($rows | Where-Object { $_.Label -like "*_at_$MapA" })
        Show-Deltas $atA 2 'round_trip' 'per round trip (MiB or count)' $columns
    }
    { $_ -in 'idle', 'fire' } {
        Show-Deltas @($rows) 2 'minute' 'per game minute (MiB or count), minute 1 left out' $columns
    }
    'save' {
        Show-Deltas @($rows | Where-Object { $_.Label -like '*quicksave_*' }) 1 'quicksave' 'per quicksave' $columns
        Show-Deltas @($rows | Where-Object { $_.Label -like '*quickload_*' }) 1 'quickload' 'per quickload' $columns
    }
    'menus' {
        Show-Deltas @($rows) 2 'round' 'per round of 7 screens' $columns
    }
    'talk' {
        Show-Deltas @($rows) 1 'step' 'per 15 game seconds of conversation' $columns
    }
}
"output in $OutDir"
