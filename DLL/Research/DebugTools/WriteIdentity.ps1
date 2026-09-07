param([string]$Library,[string]$Header)
$ErrorActionPreference='Stop'
$algorithm=[Security.Cryptography.SHA256]::Create()
$stream=[IO.File]::OpenRead($Library)
try { $hash=[BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','') }
finally { $stream.Dispose(); $algorithm.Dispose() }
$text='#pragma once' + "`n" + 'constexpr char DEBUG_TOOLS_SHA256[] = "' + $hash + '";' + "`n"
if (-not (Test-Path -LiteralPath $Header) -or [IO.File]::ReadAllText($Header) -ne $text) {
	New-Item -ItemType Directory -Path (Split-Path $Header) -Force | Out-Null
	[IO.File]::WriteAllText($Header,$text)
}
