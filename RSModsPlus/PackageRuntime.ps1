param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$destinationPath = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $destinationPath) { throw 'Choose a new output directory; existing packages are never overwritten.' }
$gui = Join-Path $repo 'Installer/Resources/RSModsGUI'
$hostFile = Join-Path $repo 'Installer/Resources/Release Public/xinput1_3.dll'
$files = @{
    'xinput1_3.dll' = $hostFile
    'RSMods/RSMods.exe' = Join-Path $gui 'RSMods.exe'
    'rsmodsplus.dll' = Join-Path $gui '../rsmodsplus.dll'
}
foreach ($file in $files.Values) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Missing public runtime build: $file" }
}
$hostBytes = [IO.File]::ReadAllBytes($hostFile)
foreach ($encoding in @([Text.Encoding]::ASCII,[Text.Encoding]::Unicode)) {
    $text = $encoding.GetString($hostBytes)
    foreach ($forbidden in @('RSModsPlus.Research','rsmodsdebug.dll','reload_probe','write_memory')) {
        if ($text.Contains($forbidden)) { throw "Public host contains developer functionality: $forbidden" }
    }
}
New-Item -ItemType Directory -Path (Join-Path $destinationPath 'RSMods') -Force | Out-Null
foreach ($name in $files.Keys) { Copy-Item -LiteralPath $files[$name] -Destination (Join-Path $destinationPath $name) }
Write-Output $destinationPath
