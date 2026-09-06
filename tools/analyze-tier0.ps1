<#
.SYNOPSIS
    Summarize (NBN TIER0) shadow-evidence lines from the game console.

.DESCRIPTION
    Tier 0 logs raw-audio Goertzel powers next to every single-note accept/reject. This
    pulls those lines and reports, per context (ACCEPT(run) / ACCEPT(matcher) / REJECT),
    how often the target frequency dominates its +/-1 semitone neighbours - the
    discrimination the semitone-binned engine features provably cannot provide. Run after
    a play session or a harness sweep; healthy enforcement criteria look like: accepts
    have tgt >> neighbours, rejects during wrong notes have a neighbour >> tgt.

.EXAMPLE
    ./tools/analyze-tier0.ps1
    ./tools/analyze-tier0.ps1 -Tail 20000
#>
[CmdletBinding()]
param([int]$Tail = 8000)

$lines = python "$PSScriptRoot\read-game-console.py" $Tail | Select-String 'NBN TIER0' -Context 0,1 |
    ForEach-Object { $_.Line + ' ' + ($_.Context.PostContext -join ' ') }

if (-not $lines) { Write-Host 'No (NBN TIER0) lines found.'; return }

$records = foreach ($line in $lines) {
    $m = [regex]::Match($line, 'TIER0\)\s+(?<ctx>\S+)\s+exp=(?<exp>\d+)\s+f=(?<f>[\d.]+)\s+tgt=(?<tgt>[\d.]+)\s+-1=(?<m1>[\d.]+)\s+\+1=(?<p1>[\d.]+)\s+-2=(?<m2>[\d.]+)\s+\+2=(?<p2>[\d.]+)\s+rms=(?<rms>[\d.]+)')
    if (-not $m.Success) { continue }
    $tgt = [double]$m.Groups['tgt'].Value
    $best = ([double]$m.Groups['m1'].Value), ([double]$m.Groups['p1'].Value),
            ([double]$m.Groups['m2'].Value), ([double]$m.Groups['p2'].Value) | Measure-Object -Maximum
    [pscustomobject]@{
        Context = $m.Groups['ctx'].Value
        Expected = [int]$m.Groups['exp'].Value
        Target = $tgt
        BestNeighbour = $best.Maximum
        TargetDominates = $tgt -gt ($best.Maximum * 2.0)
        Ratio = if ($best.Maximum -gt 0) { [math]::Round($tgt / $best.Maximum, 2) } else { [double]::PositiveInfinity }
        Rms = [double]$m.Groups['rms'].Value
    }
}

$records | Group-Object Context | ForEach-Object {
    $dom = @($_.Group | Where-Object TargetDominates).Count
    "{0}: {1} samples, target dominates (>2x best neighbour) in {2} ({3:P0})" -f `
        $_.Name, $_.Count, $dom, ($dom / [math]::Max(1, $_.Count))
}
Write-Host ''
Write-Host 'Per-note detail (median ratio target/best-neighbour):'
$records | Group-Object Context, Expected | ForEach-Object {
    $sorted = @($_.Group | Sort-Object Ratio)
    $median = $sorted[[int](($sorted.Count - 1) / 2)].Ratio
    "  {0}  n={1}  medianRatio={2}" -f $_.Name, $_.Count, $median
} | Sort-Object
