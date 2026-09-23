$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$resourceFile = Join-Path $repositoryRoot 'Installer\Properties\Resources.resx'
$resourceXml = [xml](Get-Content -LiteralPath $resourceFile -Raw)

function Get-ResourcePath([string]$name) {
    $resource = $resourceXml.Root.data | Where-Object { $_.name -eq $name }
    if ($null -eq $resource) { throw "Missing resource declaration: $name" }
    $relativePath = ([string]$resource.value).Split(';', 2)[0]
    return [IO.Path]::GetFullPath((Join-Path (Split-Path $resourceFile) $relativePath))
}

function Get-PeMachine([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "Not a PE image: $path"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 60)
    if ($peOffset -lt 0 -or $peOffset + 6 -gt $bytes.Length -or
        [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x4550) {
        throw "Invalid PE image: $path"
    }
    return [BitConverter]::ToUInt16($bytes, $peOffset + 4)
}

$expected = @{
    xinput1_3 = Join-Path $repositoryRoot 'Installer\Resources\Release Public\xinput1_3.dll'
    RocksmithAudioBridge = Join-Path $repositoryRoot 'Installer\Resources\Release\RocksmithAudioBridge.dll'
}

foreach ($name in $expected.Keys) {
    $referenced = Get-ResourcePath $name
    $authoritative = [IO.Path]::GetFullPath($expected[$name])
    if ($referenced -ne $authoritative) {
        throw "$name resource points to '$referenced', expected '$authoritative'."
    }
    if (-not (Test-Path -LiteralPath $authoritative -PathType Leaf)) {
        throw "Authoritative package input is missing: $authoritative"
    }
    if ((Get-PeMachine $authoritative) -ne 0x14C) {
        throw "$name must be an x86 PE image: $authoritative"
    }
    $hash = (Get-FileHash -LiteralPath $authoritative -Algorithm SHA256).Hash
    Write-Output ("PASS: {0} -> {1} ({2})" -f $name, $authoritative, $hash)
}

$legacyInputs = @(
    (Join-Path $repositoryRoot 'Installer\Resources\xinput1_3.dll'),
    (Join-Path $repositoryRoot 'Installer\Resources\RocksmithAudioBridge.dll')
)
foreach ($legacyInput in $legacyInputs) {
    if (Test-Path -LiteralPath $legacyInput -PathType Leaf) {
        $legacyHash = (Get-FileHash -LiteralPath $legacyInput -Algorithm SHA256).Hash
        $authoritativeName = [IO.Path]::GetFileName($legacyInput)
        $authoritativePath = if ($authoritativeName -eq 'xinput1_3.dll') { $expected.xinput1_3 } else { $expected.RocksmithAudioBridge }
        $authoritativeHash = (Get-FileHash -LiteralPath $authoritativePath -Algorithm SHA256).Hash
        if ($legacyHash -ne $authoritativeHash) {
            Write-Warning "Stale legacy package input is ignored by Resources.resx: $legacyInput differs from $authoritativePath."
            continue
        }
        Write-Warning "Legacy package input remains but matches the authoritative output: $legacyInput"
    }
}
