param([switch]$SilentDevice)
$ErrorActionPreference = 'Stop'
$outputDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\build\Tests\AudioRouting'))
$routingExecutable = Join-Path $outputDirectory 'routing_tests.exe'
if (-not (Test-Path -LiteralPath $routingExecutable)) { throw 'Build the harness first with Tests\AudioRouting\run.cmd.' }
$recordingDirectory = Join-Path $outputDirectory ([Guid]::NewGuid().ToString('N'))
$process = New-Object System.Diagnostics.Process
$process.StartInfo.FileName = $routingExecutable
$process.StartInfo.Arguments = '"' + $recordingDirectory + '"'
$process.StartInfo.WorkingDirectory = $outputDirectory
if ($SilentDevice) { $process.StartInfo.Arguments = '--silent-device ' + $process.StartInfo.Arguments }
$process.StartInfo.UseShellExecute = $false
$process.StartInfo.CreateNoWindow = $true
$process.StartInfo.RedirectStandardOutput = $true
$process.StartInfo.RedirectStandardError = $true
try {
	$null = $process.Start()
	$outputTask = $process.StandardOutput.ReadToEndAsync()
	$errorTask = $process.StandardError.ReadToEndAsync()
	if (-not $process.WaitForExit(15000)) {
		$process.Kill()
		$process.WaitForExit()
		throw 'Audio routing test timed out; stopped only the test process.'
	}
	$outputTask.Result
	$errorTask.Result
	if ($process.ExitCode -ne 0) { throw "Audio routing test failed: $($process.ExitCode)" }
}
finally { $process.Dispose() }
if (-not $SilentDevice) {
	$configurationDirectory = Join-Path $outputDirectory ([Guid]::NewGuid().ToString('N'))
	& (Join-Path $outputDirectory 'asio_config_tests.exe') $configurationDirectory
	if ($LASTEXITCODE -ne 0) { throw "ASIO configuration tests failed: $LASTEXITCODE" }
	& (Join-Path $outputDirectory 'input_mode_tests.exe') (Join-Path $outputDirectory ([Guid]::NewGuid().ToString('N')))
	if ($LASTEXITCODE -ne 0) { throw "ASIO mode tests failed: $LASTEXITCODE" }
	& (Join-Path $outputDirectory 'output_plan_tests.exe')
	if ($LASTEXITCODE -ne 0) { throw "Output apply-plan tests failed: $LASTEXITCODE" }
}
