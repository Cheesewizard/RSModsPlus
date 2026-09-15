param(
	[string]$ArtifactRoot = (Join-Path $PSScriptRoot '..\..\artifacts\nbn-hardness'),
	[switch]$AllowExpectedRegressions
)

$ErrorActionPreference = 'Stop'

function Assert-Condition([bool]$condition, [string]$message) {
	if (-not $condition) { throw $message }
}

function Get-Sha256([string]$path) {
	return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Resolve-DocumentedFile([System.IO.DirectoryInfo]$bundle, [string]$label) {
	$cleanLabel = $label.Trim('`', ' ')
	$exactPath = Join-Path $bundle.FullName $cleanLabel
	if (Test-Path -LiteralPath $exactPath -PathType Leaf) { return $exactPath }
	$patterns = @{
		'dry MP4' = '*-dry.mp4'; 'dry WAV' = '*-dry.wav'; 'focused WAV' = 'issue-*.wav'
		'three-skip focused MP4' = 'focus-*.mp4'; 'three-skip focused WAV' = 'focus-*.wav'
		'player-marked PNG' = 'user-marked-*.png'; 'RSMods debug log' = 'RSMods_debug*'
	}
	$patternKey = $patterns.Keys | Where-Object { $_ -eq $cleanLabel } | Select-Object -First 1
	if ($null -ne $patternKey) {
		$match = Get-ChildItem -LiteralPath $bundle.FullName -File -Filter $patterns[$patternKey] | Select-Object -First 1
		if ($null -ne $match) { return $match.FullName }
	}
	return $null
}

function Test-BundleFiles([System.IO.DirectoryInfo]$bundle) {
	$readmePath = Join-Path $bundle.FullName 'README.md'
	$snapshot = Get-ChildItem -LiteralPath $bundle.FullName -File -Filter 'RSMods_debug*' | Select-Object -First 1
	Assert-Condition (Test-Path -LiteralPath $readmePath) "$($bundle.Name): README.md is missing."
	Assert-Condition ($null -ne $snapshot) "$($bundle.Name): RSMods_debug snapshot is missing."

	$readmeLines = Get-Content -LiteralPath $readmePath
	$hashChecks = 0
	for ($index = 0; $index -lt $readmeLines.Count; $index++) {
		$line = $readmeLines[$index]
		$tableMatch = [regex]::Match($line, '^\|\s*([^|]+?)\s*\|\s*`?([0-9A-Fa-f]{64})`?\s*\|')
		if ($tableMatch.Success) {
			$fileName = $tableMatch.Groups[1].Value.Trim('`', ' ')
			$expectedHash = $tableMatch.Groups[2].Value.ToUpperInvariant()
			$filePath = Resolve-DocumentedFile $bundle $fileName
			Assert-Condition ($null -ne $filePath) "$($bundle.Name): README hash file '$fileName' is missing."
			Assert-Condition ((Get-Sha256 $filePath) -eq $expectedHash) "$($bundle.Name): SHA-256 mismatch for '$fileName'."
			$hashChecks++
			continue
		}

		$fieldMatch = [regex]::Match($line, '(?:Video|Audio|Runtime log snapshot):\s*`([^`]+)`')
		if ($fieldMatch.Success) {
			$fileName = $fieldMatch.Groups[1].Value
			$expectedHash = $null
			for ($lookAhead = $index + 1; $lookAhead -lt [Math]::Min($index + 6, $readmeLines.Count); $lookAhead++) {
				$hashMatch = [regex]::Match($readmeLines[$lookAhead], '[0-9A-Fa-f]{64}')
				if ($hashMatch.Success) { $expectedHash = $hashMatch.Value.ToUpperInvariant(); break }
			}
			if ($null -ne $expectedHash) {
				$filePath = Resolve-DocumentedFile $bundle $fileName
				Assert-Condition ($null -ne $filePath) "$($bundle.Name): README hash file '$fileName' is missing."
				Assert-Condition ((Get-Sha256 $filePath) -eq $expectedHash) "$($bundle.Name): SHA-256 mismatch for '$fileName'."
				$hashChecks++
			}
		}
	}
	Assert-Condition ($hashChecks -gt 0) "$($bundle.Name): README contains no verifiable SHA-256 entries."
	return @{ Readme = ($readmeLines -join "`n"); Log = (Get-Content -LiteralPath $snapshot.FullName -Raw); HashChecks = $hashChecks }
}

function Test-CaptureMapping([string]$name, [string]$readme, [string]$log) {
	if ($readme -notmatch 'output tap started' -and $readme -notmatch 'Recording time zero') { return 'SKIP (bundle has no in-log tap markers)' }
	Assert-Condition ($log -match '\(OUTPUT TAP\).*recording started') "${name}: recording start is absent from snapshot."
	Assert-Condition ($log -match '\(OUTPUT TAP\).*recording stopped') "${name}: recording stop is absent from snapshot."
	return 'PASS'
}

function Get-CaptureLog([string]$readme, [string]$log) {
	$mapping = [regex]::Match($readme, 'started at runtime elapsed `([0-9.]+)` and stopped at `([0-9.]+)`')
	if (-not $mapping.Success) { return $log }

	$start = [double]$mapping.Groups[1].Value
	$stop = [double]$mapping.Groups[2].Value
	$lines = [System.Collections.Generic.List[string]]::new()
	foreach ($line in ($log -split "`r?`n")) {
		$timestamp = [regex]::Match($line, '^([0-9]+\.[0-9]+)')
		if (-not $timestamp.Success) { continue }
		$elapsed = [double]$timestamp.Groups[1].Value
		if ($elapsed -ge $start -and $elapsed -le $stop) { $lines.Add($line) }
	}
	return $lines -join "`n"
}

function Test-SpeakerRoute([string]$name, [string]$readme, [string]$log) {
	if ($readme -notmatch 'Speaker Mode \+1') { return 'SKIP (not a Speaker Mode +1 bundle)' }
	Assert-Condition ($readme -match 'Eb -> E \(\+1\)') "${name}: README route declaration is missing."
	Assert-Condition ($readme -match 'inputShift=0') "${name}: README does not establish synchronized detector frames."
	Assert-Condition ($log -match 'capture=RS_ASIO') "${name}: RS_ASIO route is absent from snapshot."
	Assert-Condition ($log -match 'inputShift=0') "${name}: synchronized inputShift=0 evidence is absent."
	return 'PASS'
}

$expectedFailures = [System.Collections.Generic.List[string]]::new()
$bundles = Get-ChildItem -LiteralPath ([IO.Path]::GetFullPath($ArtifactRoot)) -Directory | Sort-Object Name
Assert-Condition ($bundles.Count -gt 0) "No artifact bundles found at '$ArtifactRoot'."

foreach ($bundle in $bundles) {
	$data = Test-BundleFiles $bundle
	$mapping = Test-CaptureMapping $bundle.Name $data.Readme $data.Log
	$route = Test-SpeakerRoute $bundle.Name $data.Readme $data.Log
	$captureLog = Get-CaptureLog $data.Readme $data.Log

	$fastCarryOver = [regex]::Matches($captureLog, '\(NBN FLOW TIMING\) commit-to-commit (2[0-4][0-9]\.[0-9]+) ms')
	if ($bundle.Name -match 'rock-and-roll-all-nite' -and $fastCarryOver.Count -gt 0) {
		$gaps = ($fastCarryOver | ForEach-Object { $_.Groups[1].Value }) -join ', '
		$expectedFailures.Add("$($bundle.Name): dense chord successors committed before confirmed release; captured gaps ${gaps} ms.")
	}
	$muteAccepts = [regex]::Matches($captureLog, '\(NBN FRET MUTE\) Fresh attack accepted without pitched-chord matching')
	if ($bundle.Name -match 'pride-and-joy' -and $muteAccepts.Count -gt 0) {
		$expectedFailures.Add("$($bundle.Name): $($muteAccepts.Count) fret-hand-muted targets accepted without pitched-chord matching.")
	}

	Write-Output ("PASS {0}: hashes={1}, capture={2}, speaker-route={3}" -f $bundle.Name, $data.HashChecks, $mapping, $route)
}

if ($expectedFailures.Count -gt 0) {
	Write-Output ''
	Write-Output 'REGRESSION FAILURES:'
	$expectedFailures | ForEach-Object { Write-Output ("FAIL $_") }
	if (-not $AllowExpectedRegressions) { exit 1 }
	Write-Output 'Expected baseline failures allowed by -AllowExpectedRegressions.'
}
else {
	Write-Output 'PASS: no documented Note-by-Note regression signatures found.'
}

exit 0
