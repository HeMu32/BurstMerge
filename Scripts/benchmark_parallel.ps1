param(
    [string]$Cli = "C:\Users\HeMu\Desktop\BurstMerge\Build\apps\cli\burstmerge_cli.exe",
    [string]$OutDir = "Z:\TEMP\benchmark_parallel",
    [switch]$SkipSerial,
    [switch]$SkipParallel
)

$ErrorActionPreference = 'Stop'

if (!(Test-Path -LiteralPath $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}

function Invoke-BenchmarkCase {
    param(
        [string]$Name,
        [string[]]$Inputs,
        [string[]]$ExtraArgs,
        [bool]$Serial
    )

    if ($Serial) {
        $env:BURSTMERGE_THREADS = '1'
    } else {
        Remove-Item Env:BURSTMERGE_THREADS -ErrorAction SilentlyContinue
    }

    $outPath = Join-Path $OutDir ($Name + ($(if ($Serial) { '_serial.dng' } else { '_mt.dng' })))
    $args = @()
    foreach ($input in $Inputs) {
        $args += '-i'
        $args += $input
    }
    $args += '-o'
    $args += $outPath
    $args += $ExtraArgs

    $output = & $Cli @args 2>&1
    $text = ($output | Out-String)
    $match = [regex]::Match($text, 'Finished in ([0-9]+(?:\.[0-9]+)?)s')
    if (!$match.Success) {
        throw "Failed to parse runtime for case '$Name'"
    }

    [pscustomobject]@{
        Name = $Name
        Mode = $(if ($Serial) { 'serial' } else { 'mt' })
        Seconds = [double]$match.Groups[1].Value
        Output = $outPath
    }
}

$seq1 = Get-ChildItem -LiteralPath 'Z:\seq1' | ForEach-Object { $_.FullName }
$seq2 = Get-ChildItem -LiteralPath 'Z:\seq2' | ForEach-Object { $_.FullName }

$cases = @(
    @{ Name = 'seq1_legacy_spatial'; Inputs = $seq1; Args = @('--alignment', 'legacy') },
    @{ Name = 'seq1_dense_spatial'; Inputs = $seq1; Args = @('--alignment', 'dense') },
    @{ Name = 'seq1_freq_spatial'; Inputs = $seq1; Args = @('--alignment', 'freq') },
    @{ Name = 'seq1_legacy_wiener_legacy'; Inputs = $seq1; Args = @('--alignment', 'legacy', '--merge-algo', 'frequency', '--frequency-mode', 'wiener-legacy') },
    @{ Name = 'seq1_legacy_wiener'; Inputs = $seq1; Args = @('--alignment', 'legacy', '--merge-algo', 'frequency', '--frequency-mode', 'wiener') },
    @{ Name = 'seq2_legacy_spatial'; Inputs = $seq2; Args = @('--alignment', 'legacy') },
    @{ Name = 'seq2_dense_spatial'; Inputs = $seq2; Args = @('--alignment', 'dense') },
    @{ Name = 'seq2_freq_spatial'; Inputs = $seq2; Args = @('--alignment', 'freq') },
    @{ Name = 'seq2_legacy_wiener_legacy'; Inputs = $seq2; Args = @('--alignment', 'legacy', '--merge-algo', 'frequency', '--frequency-mode', 'wiener-legacy') },
    @{ Name = 'seq2_legacy_wiener'; Inputs = $seq2; Args = @('--alignment', 'legacy', '--merge-algo', 'frequency', '--frequency-mode', 'wiener') }
)

$results = @()
foreach ($case in $cases) {
    if (-not $SkipSerial) {
        $results += Invoke-BenchmarkCase -Name $case.Name -Inputs $case.Inputs -ExtraArgs $case.Args -Serial $true
    }
    if (-not $SkipParallel) {
        $results += Invoke-BenchmarkCase -Name $case.Name -Inputs $case.Inputs -ExtraArgs $case.Args -Serial $false
    }
}

$results | Format-Table -AutoSize
