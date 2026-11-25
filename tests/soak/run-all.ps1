# Runs every memory soak scenario, each in a fresh process, -Repeats times, and writes
# each run's output plus a combined SUMMARY file to -OutDir.
#
#   pwsh tests/soak/run-all.ps1 -Bin build/release/bin/Release -OutDir soak-results
param(
    [Parameter(Mandatory = $true)][string]$Bin,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$Repeats = 3,
    [int]$ExeRepeats = 2,
    [switch]$SkipExe
)

$ErrorActionPreference = 'Stop'
$Bin = (Resolve-Path $Bin).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$tests = Join-Path $Bin 'unit-tests.exe'
if (-not (Test-Path $tests)) { $tests = Join-Path $Bin 'unit-tests' }
$exe = Join-Path $Bin 'ambrosia.exe'
if (-not (Test-Path $exe)) { $exe = Join-Path $Bin 'ambrosia' }
$soak = Join-Path $PSScriptRoot 'exe-soak.ps1'

$engine = @(
    @{ Name = 'travel'; Tag = '[soak-travel]'; Env = @{ SOAK_ITERATIONS = '12' } },
    @{ Name = 'travel-small'; Tag = '[soak-travel-small]'; Env = @{ SOAK_ITERATIONS = '12' } },
    @{ Name = 'save'; Tag = '[soak-save]'; Env = @{ SOAK_ITERATIONS = '15' } },
    @{ Name = 'idle-market'; Tag = '[soak-idle]'; Env = @{ SOAK_MINUTES = '10'; SOAK_MAP = '06_HongKong_WanChai_Market' } },
    @{ Name = 'idle-island'; Tag = '[soak-idle]'; Env = @{ SOAK_MINUTES = '10'; SOAK_MAP = '01_NYC_UNATCOIsland' } },
    @{ Name = 'idle-street'; Tag = '[soak-idle]'; Env = @{ SOAK_MINUTES = '10'; SOAK_MAP = '04_NYC_Street' } },
    @{ Name = 'idle-canal'; Tag = '[soak-idle]'; Env = @{ SOAK_MINUTES = '10'; SOAK_MAP = '06_HongKong_WanChai_Canal' } },
    @{ Name = 'fire'; Tag = '[soak-fire]'; Env = @{ SOAK_MINUTES = '10' } },
    @{ Name = 'churn'; Tag = '[soak-churn]'; Env = @{ SOAK_MINUTES = '5'; SOAK_RATE = '60' } },
    @{ Name = 'sounds'; Tag = '[soak-sounds]'; Env = @{} }
)

$known = 'SOAK_ITERATIONS', 'SOAK_MINUTES', 'SOAK_MAP', 'SOAK_RATE', 'SOAK_STAY_SECONDS', 'SOAK_SLOTS'
for ($r = 1; $r -le $Repeats; $r++) {
    foreach ($run in $engine) {
        foreach ($k in $known) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
        foreach ($k in $run.Env.Keys) { Set-Item "Env:$k" $run.Env[$k] }
        $file = Join-Path $OutDir ("engine-{0}-{1}.txt" -f $run.Name, $r)
        $time = Measure-Command { & $tests $run.Tag *> $file }
        "engine $($run.Name) #$r $([int]$time.TotalSeconds)s exit $LASTEXITCODE"
    }
}
foreach ($k in $known) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }

if (-not $SkipExe) {
    $exeRuns = @(
        @{ Name = 'travel'; Args = @('-Scenario', 'travel', '-Iterations', '10') },
        @{ Name = 'travel-small'; Args = @('-Scenario', 'travel', '-Iterations', '10', '-MapA', '02_NYC_Street', '-MapB', '02_NYC_Bar') },
        @{ Name = 'save'; Args = @('-Scenario', 'save', '-Iterations', '10', '-Map', '01_NYC_UNATCOIsland') },
        @{ Name = 'idle-market'; Args = @('-Scenario', 'idle', '-Minutes', '10', '-Map', '06_HongKong_WanChai_Market') },
        @{ Name = 'fire'; Args = @('-Scenario', 'fire', '-Minutes', '5', '-Map', '01_NYC_UNATCOIsland') },
        @{ Name = 'menus'; Args = @('-Scenario', 'menus', '-Iterations', '40', '-Map', '01_NYC_UNATCOIsland') },
        @{ Name = 'talk'; Args = @('-Scenario', 'talk', '-Minutes', '3') }
    )
    for ($r = 1; $r -le $ExeRepeats; $r++) {
        foreach ($run in $exeRuns) {
            $dir = Join-Path $OutDir ("exe-{0}-{1}" -f $run.Name, $r)
            $file = "$dir.txt"
            $time = Measure-Command { & pwsh -NoProfile -File $soak -Exe $exe @($run.Args) -OutDir $dir *> $file }
            "exe $($run.Name) #$r $([int]$time.TotalSeconds)s"
        }
    }
}

Get-ChildItem $OutDir -Filter '*.txt' | Sort-Object Name | ForEach-Object {
    $name = $_.BaseName
    Get-Content $_.FullName | Where-Object { $_ -match '^(SUMMARY|TICK|FAILED)|test cases:' } | ForEach-Object { "$name,$_" }
} | Set-Content (Join-Path $OutDir 'summary.txt')
"summary in $(Join-Path $OutDir 'summary.txt')"
