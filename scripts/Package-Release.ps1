[CmdletBinding()]
param(
	[string]$OutputPath = (Join-Path $PSScriptRoot "..\artifacts\RSModsPlus.zip")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$resolvedOutputPath = [System.IO.Path]::GetFullPath($OutputPath)
$releaseFiles = [ordered]@{
	"xinput1_3.dll" = Join-Path $repositoryRoot "Installer\Resources\xinput1_3.dll"
	"RSMods/RSMods.exe" = Join-Path $repositoryRoot "Installer\Resources\RSModsGUI\RSMods.exe"
	"RSMods/RSMods.exe.config" = Join-Path $repositoryRoot "Installer\Resources\RSModsGUI\RSMods.exe.config"
}

foreach ($sourcePath in $releaseFiles.Values)
{
	if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf))
	{
		throw "Required release file is missing: $sourcePath. Build the DLL and GUI Release configurations before packaging."
	}
}

$helperPath = $releaseFiles["RSMods/RSMods.exe"]
$helperText = [System.Text.Encoding]::Unicode.GetString([System.IO.File]::ReadAllBytes($helperPath))
if (-not $helperText.Contains("--speaker-cache-extract"))
{
	throw "RSMods.exe does not contain the Speaker Mode extraction command. Rebuild the GUI from this source revision."
}

$outputDirectory = Split-Path -Parent $resolvedOutputPath
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container))
{
	New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$fileStream = [System.IO.File]::Open(
	$resolvedOutputPath,
	[System.IO.FileMode]::Create,
	[System.IO.FileAccess]::Write,
	[System.IO.FileShare]::None)

try
{
	$archive = [System.IO.Compression.ZipArchive]::new(
		$fileStream,
		[System.IO.Compression.ZipArchiveMode]::Create,
		$false)

	try
	{
		foreach ($entry in $releaseFiles.GetEnumerator())
		{
			[System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
				$archive,
				$entry.Value,
				$entry.Key,
				[System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
		}
	}
	finally
	{
		$archive.Dispose()
	}
}
finally
{
	$fileStream.Dispose()
}

$verificationArchive = [System.IO.Compression.ZipFile]::OpenRead($resolvedOutputPath)
try
{
	$actualEntries = @($verificationArchive.Entries | ForEach-Object FullName)
	$expectedEntries = @($releaseFiles.Keys)
	if (Compare-Object -ReferenceObject $expectedEntries -DifferenceObject $actualEntries)
	{
		throw "Release package contents do not match the required file list."
	}
}
finally
{
	$verificationArchive.Dispose()
}

Write-Output $resolvedOutputPath
