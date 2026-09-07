param(
	[ValidateRange(1, 1000)]
	[int]$Repeat = 1,
	[string[]]$Scenario = @(
		'startup-liveness', 'neutral-passthrough', 'neutral-int16', 'neutral-int24',
		'neutral-float32', 'stopped-readiness', 'concurrent-rebind', 'multiple-routes',
		'rejected-format', 'buffer-results', 'active-shift', 'concurrent-players', 'late-attachment',
		'lifecycle-forwarding', 'lifecycle-overflow'
	)
)

$ErrorActionPreference = 'Stop'
$outputDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\build\Tests\AudioCapture'))
$executable = Join-Path $outputDirectory 'capture_tests.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw 'Build the harness first with Tests\AudioCapture\run.cmd.' }
$passed = 0
foreach ($iteration in 1..$Repeat) {
	foreach ($test in $Scenario) {
		if ($test -notmatch '^[a-z0-9-]+$') { throw "Invalid scenario name: $test" }
		$stdout = Join-Path $outputDirectory "$test.stdout.txt"
		$stderr = Join-Path $outputDirectory "$test.stderr.txt"
		$process = New-Object System.Diagnostics.Process
		$process.StartInfo.FileName = $executable
		$process.StartInfo.Arguments = $test
		$process.StartInfo.WorkingDirectory = $outputDirectory
		$process.StartInfo.UseShellExecute = $false
		$process.StartInfo.CreateNoWindow = $true
		$process.StartInfo.RedirectStandardOutput = $true
		$process.StartInfo.RedirectStandardError = $true
		$null = $process.Start()
		$outputTask = $process.StandardOutput.ReadToEndAsync()
		$errorTask = $process.StandardError.ReadToEndAsync()
		try {
			if (-not $process.WaitForExit(15000)) {
				$process.Kill()
				$process.WaitForExit()
				throw "TIMEOUT: $test (iteration $iteration). Only the test process was stopped."
			}
			[IO.File]::WriteAllText($stdout, $outputTask.Result)
			[IO.File]::WriteAllText($stderr, $errorTask.Result)
			if ($process.ExitCode -ne 0) {
				Get-Content -LiteralPath $stdout, $stderr
				throw "FAIL: $test (iteration $iteration), exit $($process.ExitCode)"
			}
			++$passed
		}
		finally { $process.Dispose() }
	}
}
"PASS: $passed scenario runs ($($Scenario.Count) scenarios x $Repeat repetitions)."
