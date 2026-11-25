# Turns a run-all.ps1 results folder into per-source figures: for each scenario, the mean
# change per step of private bytes, live heap bytes, retired package images, object
# property data, decoded sounds, renderer cache bytes and destroyed windows.
#
#   pwsh tests/soak/analyze.ps1 -Results soak-results
param([Parameter(Mandatory = $true)][string]$Results)

$ErrorActionPreference = 'Stop'

function Mean([double[]]$v) { if ($v.Count -eq 0) { return [double]::NaN }; ($v | Measure-Object -Average).Average }
function Sd([double[]]$v) {
    if ($v.Count -lt 2) { return 0 }
    $m = Mean $v
    [math]::Sqrt((($v | ForEach-Object { ($_ - $m) * ($_ - $m) }) | Measure-Object -Sum).Sum / ($v.Count - 1))
}

# Engine rows: SOAK,scenario,iteration,label,game_s,private_mib,table,objects,destroyed,
# data_mib,destroyed_data_mib,actors,destroy_count,linker_mib,retired,retired_mib,sound_mib,sounds,heap_mib
function Engine-Rows([string]$file) {
    Get-Content $file | Where-Object { $_ -match '^SOAK,' -and $_ -notmatch '^SOAK,scenario' } | ForEach-Object {
        $p = $_ -split ','
        [pscustomobject]@{
            Label = $p[3]; Private = [double]$p[5]; Table = [double]$p[6]; Destroyed = [double]$p[8]
            Data = [double]$p[9]; DestroyedData = [double]$p[10]; DestroyCount = [double]$p[12]
            Retired = [double]$p[15]; Sound = [double]$p[16]; Heap = [double]$p[18]
        }
    }
}

# Deltas between consecutive rows of a set, from index $from.
function Steps([object[]]$rows, [int]$from) {
    for ($k = [math]::Max($from, 1); $k -lt $rows.Count; $k++) {
        $a = $rows[$k - 1]; $b = $rows[$k]
        [pscustomobject]@{
            Private = $b.Private - $a.Private; Heap = $b.Heap - $a.Heap; Retired = $b.Retired - $a.Retired
            Data = $b.Data - $a.Data; Table = $b.Table - $a.Table; Destroys = $b.DestroyCount - $a.DestroyCount
            DestroyedData = $b.DestroyedData - $a.DestroyedData
        }
    }
}

$table = [System.Collections.Generic.List[object]]::new()
function Add-Line([string]$scenario, [string]$unit, [object[]]$steps) {
    if (-not $steps -or $steps.Count -eq 0) { return }
    $private = @($steps | ForEach-Object Private); $heap = @($steps | ForEach-Object Heap)
    $retired = @($steps | ForEach-Object Retired); $data = @($steps | ForEach-Object Data)
    $hm = Mean $heap; $rm = Mean $retired; $dm = Mean $data
    $table.Add([pscustomobject]@{
            Scenario   = $scenario; Unit = $unit; N = $steps.Count
            PrivateMiB = ('{0:F2} +- {1:F2}' -f @((Mean $private), (Sd $private)))
            HeapMiB    = ('{0:F2} +- {1:F2}' -f @($hm, (Sd $heap)))
            Images     = '{0:F2}' -f $rm
            ObjData    = '{0:F2}' -f $dm
            OtherCpp   = '{0:F2}' -f ($hm - $rm - $dm)
            Slack      = '{0:F2}' -f ((Mean $private) - $hm)
            Objects    = '{0:F0}' -f (Mean @($steps | ForEach-Object Table))
        })
}

foreach ($file in Get-ChildItem $Results -Filter 'engine-*.txt' | Sort-Object Name) {
    $rows = @(Engine-Rows $file.FullName)
    if ($rows.Count -eq 0) { continue }
    $name = $file.BaseName
    switch -Wildcard ($name) {
        'engine-travel*' {
            $first = $rows[1].Label
            $atFirst = @($rows | Where-Object Label -eq $first)
            Add-Line $name 'round trip' @(Steps $atFirst 2)
        }
        'engine-save*' {
            Add-Line "$name quicksave" 'quicksave' @(Steps @($rows | Where-Object Label -eq 'quicksave') 1)
            Add-Line "$name quickload" 'quickload' @(Steps @($rows | Where-Object Label -eq 'quickload') 1)
            Add-Line "$name list" 'listing of 8 slots' @(Steps @($rows | Where-Object Label -like 'list_*') 1)
        }
        { $_ -like 'engine-idle*' -or $_ -like 'engine-fire*' -or $_ -like 'engine-churn*' } {
            $minutes = @($rows | Where-Object { $_.Label -in 'start', 'minute' })
            $steps = @(Steps $minutes 1)
            Add-Line $name 'game minute' $steps
            $destroys = (@($steps | ForEach-Object Destroys) | Measure-Object -Sum).Sum
            if ($destroys -gt 100) {
                $heap = (@($steps | ForEach-Object Heap) | Measure-Object -Sum).Sum
                $private = (@($steps | ForEach-Object Private) | Measure-Object -Sum).Sum
                $data = (@($steps | ForEach-Object DestroyedData) | Measure-Object -Sum).Sum
                $objects = (@($steps | ForEach-Object Table) | Measure-Object -Sum).Sum
                "{0}: per destroyed actor {1:F2} KiB private, {2:F2} KiB heap, {3:F2} KiB property data, {4:F2} objects ({5} destroyed)" -f $name, ($private * 1024 / $destroys), ($heap * 1024 / $destroys), ($data * 1024 / $destroys), ($objects / $destroys), $destroys
            }
        }
    }
}
'Engine, per step. Heap = live heap bytes; Images = retired package file images; ObjData = object property data; OtherCpp = Heap - Images - ObjData; Slack = Private - Heap (heap kept for reuse, fragmentation, non-heap)'
$table | Format-Table -AutoSize | Out-String -Width 250

# Exe marks.csv: deltas per step of each counter column.
$columns = 'PrivateMiB', 'table', 'retired_bytes', 'data', 'sound_decoded', 'sounds', 'meshdraw_bytes', 'pose_bytes', 'light_bytes', 'meshdraw_dead', 'queries', 'destroyed_windows', 'destroyed_window_data'
$exe = foreach ($dir in Get-ChildItem $Results -Directory -Filter 'exe-*' | Sort-Object Name) {
    $csv = Join-Path $dir.FullName 'marks.csv'
    if (-not (Test-Path $csv)) { continue }
    $rows = @(Import-Csv $csv)
    $sets = switch -Wildcard ($dir.Name) {
        'exe-travel*' { $l = $rows[0].Label.Substring(4); @{ 'round trip' = @(@($rows | Where-Object { $_.Label.Substring(4) -eq $l }) | Select-Object -Skip 1) } }
        'exe-save*' { @{ quicksave = @($rows | Where-Object Label -like '*quicksave_*'); quickload = @($rows | Where-Object Label -like '*quickload_*') } }
        'exe-idle*' { @{ 'game minute' = @($rows | Select-Object -Skip 1) } }
        'exe-fire*' { @{ 'game minute' = @($rows | Select-Object -Skip 1) } }
        'exe-menus*' { @{ 'round of 7 screens' = @($rows | Select-Object -Skip 1) } }
        'exe-talk*' { @{ '15 s of conversation' = @($rows) } }
    }
    foreach ($unit in $sets.Keys) {
        $set = $sets[$unit]
        if ($set.Count -lt 2) { continue }
        $row = [ordered]@{ Run = $dir.Name; Unit = $unit; N = $set.Count - 1 }
        foreach ($c in $columns) {
            $d = @(for ($k = 1; $k -lt $set.Count; $k++) { [double]$set[$k].$c - [double]$set[$k - 1].$c })
            $row[$c] = '{0:F2}' -f (Mean $d)
        }
        [pscustomobject]$row
    }
}
'Exe, mean change per step (MiB for bytes columns, counts otherwise)'
$exe | ConvertTo-Csv -NoTypeInformation -UseQuotes Never
