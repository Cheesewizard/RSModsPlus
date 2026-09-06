[CmdletBinding()]
param(
	[ValidateSet('ping', 'status', 'events', 'watch', 'reload', 'unload', 'enable', 'disable',
		'grid-gate', 'highway', 'render-snapshot', 'note-list', 'screen-map', 'probe-ping',
		'probe-peek', 'neck-placement', 'physical-marker-draws',
		'guide-suppress-on', 'guide-suppress-off', 'marker-dim-on', 'marker-dim-off',
		'marker-persist-on', 'marker-persist-off', 'marker-persist-status',
		'transition-capture', 'transition-capture-status', 'transition-capture-off',
		'full-draw-feed-on', 'full-draw-feed-off',
		'native-draw-feed-on', 'native-draw-feed-off', 'frametime', 'probe-menu-rows',
		'stale-filter-on', 'stale-filter-off',
		'heap-check-on', 'heap-check-off',
		'chord-holds-on', 'chord-holds-off',
		'chord-window-on', 'chord-window-off',
		'repeat-holds-on', 'repeat-holds-off',
		'freeze-flag-on', 'freeze-flag-off',
		'chord-panel-on', 'chord-panel-off',
		'safety-release-on', 'safety-release-off',
		'schedule-shift-on', 'schedule-shift-off',
		'updim-on', 'updim-off',
		'native-seek-test',
		'native-release-on', 'native-release-off',
		'nd-accept-on', 'nd-accept-off',
		'freeze-mode-test',
		'ml-pitch',
		'read-memory', 'write-memory', 'watch-memory', 'unwatch-memory')]
	[string]$Command = 'status',

	[string]$ProbePath,

	# Accepts decimal or hex ('0x7E2880'); hex strings are passed through to the bridge as-is.
	[string]$Address,

	[ValidateRange(1, 65536)]
	[int]$Length = 64,

	# write-memory only: the bytes to write as an even-length hex string ('90c3').
	# The response carries the previous bytes, so a patch reverts by writing them back.
	[string]$Bytes,

	# watch-memory / unwatch-memory
	[string]$Label,
	[UInt64]$WatchId,
	[ValidateRange(10, 10000)]
	[int]$WatchIntervalMs = 100,
	[switch]$All,

	# grid-gate only. The neck-diagram grid gate at 0x7AA140 is settable at runtime because
	# its detour is installed at startup and cannot be hot-reloaded.
	#   off      leave the diagram exactly as Rocksmith draws it
	#   restore  run the original write, then put the cell back as it was
	#   sentinel run the original write, then overwrite the cell with -GridSentinelValue/Key
	# grid-gate:   off | restore | sentinel
	# highway:     off | all | near-target
	# neck-placement: off | dry | target | window
	[ValidateSet('off', 'restore', 'sentinel', 'all', 'near-target', 'x0', 'target', 'dry',
		'window')]
	[string]$Mode,

	# highway near-target only: half-width of the window in seconds, either side of the target.
	[ValidateRange(0.0, 5.0)]
	[float]$WindowSeconds,

	[UInt32]$GridSentinelValue,

	# neck-placement only: bitmask over the placement sites target mode may act on
	# (indices over {0x7A8B10, 0x7A8CE0, 0x7A8E90, 0x7A90B0, 0x7A9290, 0x7A93C0, 0x7A94F0};
	# sites 1 and 2 are the highway - keep them out).
	[UInt32]$SiteMask,

	# neck-placement only: arm bounded (NBN FADE) and (NBN FRET ELEMENT) samples over
	# the fade pipeline and per-element resource-transform seam. Independent of -Mode;
	# re-issue to re-arm.
	[switch]$FadeDry,

	[float]$GridSentinelKey,

	# guide-suppress-on only: the mesh signature to suppress, from a physical-marker-draw
	# event (stride, vertexCount, primitiveCount). All three are required to enable.
	[UInt32]$SuppressStride = 0,
	[UInt32]$SuppressVerts = 0,
	[UInt32]$SuppressPrims = 0,

	# transition-capture only: how many target transitions one arm captures before
	# disarming itself.
	[ValidateRange(1, 16)]
	[int]$TransitionCount = 1,

	[UInt64]$After = 0,

	[ValidateRange(50, 10000)]
	[int]$IntervalMilliseconds = 250
)

$ErrorActionPreference = 'Stop'

function Invoke-ResearchBridgeRequest
{
	param(
		[Parameter(Mandatory)]
		[hashtable]$Request
	)

	$pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
		'.',
		'RSModsPlus.Research',
		[System.IO.Pipes.PipeDirection]::InOut,
		[System.IO.Pipes.PipeOptions]::None)

	try
	{
		$pipe.Connect(2000)
		$writer = [System.IO.StreamWriter]::new($pipe, [System.Text.UTF8Encoding]::new($false), 4096, $true)
		$reader = [System.IO.StreamReader]::new($pipe, [System.Text.UTF8Encoding]::new($false), $false, 4096, $true)
		try
		{
			$writer.AutoFlush = $true
			$writer.WriteLine(($Request | ConvertTo-Json -Compress -Depth 8))
			$responseLine = $reader.ReadLine()
			if ([string]::IsNullOrWhiteSpace($responseLine))
			{
				throw 'The research bridge closed the pipe without returning a response.'
			}
			$writer.WriteLine('ack')
			return $responseLine | ConvertFrom-Json
		}
		finally
		{
			try
			{
				$reader.Dispose()
			}
			catch [System.IO.IOException]
			{
			}
			try
			{
				$writer.Dispose()
			}
			catch [System.IO.IOException]
			{
			}
		}
	}
	finally
	{
		$pipe.Dispose()
	}
}

function ConvertTo-AddressValue
{
	param(
		[Parameter(Mandatory)]
		[string]$Text
	)

	if ($Text -match '^0[xX]')
	{
		return [Convert]::ToUInt64($Text.Substring(2), 16)
	}
	return [UInt64]$Text
}

function Assert-Success
{
	param(
		[Parameter(Mandatory)]
		[object]$Response
	)

	if (-not $Response.ok)
	{
		throw "Research bridge command failed: $($Response.error)"
	}
	return $Response
}

switch ($Command)
{
	'ping'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{ command = 'ping' })
		break
	}
	'status'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{ command = 'status' })
		break
	}
	'events'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'events'
			after = $After
			maximum = 64
		})
		break
	}
	'watch'
	{
		$cursor = $After
		while ($true)
		{
			$response = Assert-Success (Invoke-ResearchBridgeRequest @{
				command = 'events'
				after = $cursor
				maximum = 64
			})
			foreach ($event in $response.events)
			{
				$event | ConvertTo-Json -Compress -Depth 8
			}
			$cursor = [UInt64]$response.latest
			Start-Sleep -Milliseconds $IntervalMilliseconds
		}
	}
	'reload'
	{
		$request = @{ command = 'reload_probe' }
		if (-not [string]::IsNullOrWhiteSpace($ProbePath))
		{
			$request.path = [System.IO.Path]::GetFullPath($ProbePath)
		}
		Assert-Success (Invoke-ResearchBridgeRequest $request)
		break
	}
	'unload'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{ command = 'unload_probe' })
		break
	}
	'enable'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'set_note_by_note_enabled'
			enabled = $true
		})
		break
	}
	'disable'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'set_note_by_note_enabled'
			enabled = $false
		})
		break
	}
	'render-snapshot'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'arm_render_snapshot'
		})
		break
	}
	'physical-marker-draws'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'arm_physical_marker_draws'
		})
		break
	}
	'guide-suppress-on'
	{
		if ($SuppressStride -eq 0 -or $SuppressVerts -eq 0 -or $SuppressPrims -eq 0)
		{
			throw 'guide-suppress-on requires -SuppressStride, -SuppressVerts and -SuppressPrims.'
		}
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'set_physical_guide_suppression'
			enabled = $true
			stride = $SuppressStride
			vertexCount = $SuppressVerts
			primitiveCount = $SuppressPrims
		})
		break
	}
	'guide-suppress-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'set_physical_guide_suppression'
			enabled = $false
		})
		break
	}
	'note-list'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'arm_note_draw_list'
		})
		break
	}
	'screen-map'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'arm_screen_map'
		})
		break
	}
	'probe-ping'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_ping'
		})
		break
	}
	'chord-holds-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_chord_holds_on'
		})
		break
	}
	'safety-release-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_safety_release_on'
		})
		break
	}
	'schedule-shift-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_schedule_shift_on'
		})
		break
	}
	'native-seek-test'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_native_seek_test'
		})
		break
	}
	'native-release-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_native_release_on'
		})
		break
	}
	'freeze-mode-test'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_freeze_mode_test'
		})
		break
	}
	'ml-pitch'
	{
		# Tier-1 companion observation: alive plus the latest midi/confidence/ageSeconds
		# when an estimate exists (midi is in the observed route frame).
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'ml_pitch'
		})
		break
	}
	'nd-accept-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_nd_accept_on'
		})
		break
	}
	'nd-accept-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_nd_accept_off'
		})
		break
	}
	'native-release-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_native_release_off'
		})
		break
	}
	'updim-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'set_upcoming_dim'
			enabled = $true
		})
		break
	}
	'updim-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'set_upcoming_dim'
			enabled = $false
		})
		break
	}
	'schedule-shift-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_schedule_shift_off'
		})
		break
	}
	'safety-release-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_safety_release_off'
		})
		break
	}
	'chord-holds-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_chord_holds_off'
		})
		break
	}
	'chord-window-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_chord_window_on'
		})
		break
	}
	'chord-window-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_chord_window_off'
		})
		break
	}
	'repeat-holds-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_repeat_holds_on'
		})
		break
	}
	'repeat-holds-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_repeat_holds_off'
		})
		break
	}
	'freeze-flag-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_freeze_flag_on'
		})
		break
	}
	'freeze-flag-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_freeze_flag_off'
		})
		break
	}
	'chord-panel-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_chord_panel_on'
		})
		break
	}
	'chord-panel-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_chord_panel_off'
		})
		break
	}
	'marker-dim-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_marker_dim_on'
		})
		break
	}
	'marker-dim-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_marker_dim_off'
		})
		break
	}
	'marker-persist-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_marker_persist_on'
		})
		break
	}
	'marker-persist-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_marker_persist_off'
		})
		break
	}
	'marker-persist-status'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_marker_persist_status'
		})
		break
	}
	'transition-capture'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_transition_capture_arm'
			transitions = $TransitionCount
		})
		break
	}
	'transition-capture-status'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_transition_capture_status'
		})
		break
	}
	'transition-capture-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_transition_capture_disarm'
		})
		break
	}
	'heap-check-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_heap_check_on'
		})
		break
	}
	'heap-check-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_heap_check_off'
		})
		break
	}
	'stale-filter-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'set_stale_marker_filter'
			enabled = $true
		})
		break
	}
	'stale-filter-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'set_stale_marker_filter'
			enabled = $false
		})
		break
	}
	'full-draw-feed-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'set_full_draw_feed'
			enabled = $true
		})
		break
	}
	# Ask the UI thread to enumerate the current Riff Repeater screen's row container, then read it back.
	'probe-menu-rows'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{ command = 'probe_menu_rows' }) | Out-Null
		Start-Sleep -Milliseconds 400
		$state = Assert-Success (Invoke-ResearchBridgeRequest @{ command = 'status' })
		$state.riffRepeaterMenu
		break
	}
	'native-draw-feed-on'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'set_native_draw_feed'
			enabled = $true
		})
		break
	}
	'native-draw-feed-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'set_native_draw_feed'
			enabled = $false
		})
		break
	}
	# Measured frame time from the host's EndScene seam: prints the average / worst-of-120
	# frame and the derived FPS alongside the state that explains it (in song, NBN on, feeds).
	'frametime'
	{
		$state = Assert-Success (Invoke-ResearchBridgeRequest @{ command = 'status' })
		if ($state.probeConfigurationMismatch)
		{
			Write-Warning 'PROBE/HOST CONFIGURATION MISMATCH: this frame-rate reading is not valid. Rebuild both in one configuration.'
		}
		[pscustomobject]@{
			fps = [math]::Round($state.frameTime.fps, 1)
			averageMs = [math]::Round($state.frameTime.averageMs, 2)
			windowMaxMs = [math]::Round($state.frameTime.windowMaxMs, 2)
			frames = $state.frameTime.frames
			inSong = $state.gameState.inSong
			noteByNoteEnabled = $state.noteByNoteEnabled
			probeLoaded = $state.probeLoaded
			probeIsDebugBuild = $state.probeIsDebugBuild
			hostIsDebugBuild = $state.hostIsDebugBuild
			probeConfigurationMismatch = $state.probeConfigurationMismatch
			probeSourceHash = $state.probeSourceHash
			fullDrawFeed = $state.fullDrawFeedEnabled
			nativeDrawFeed = $state.nativeDrawFeedEnabled
		}
		break
	}
	'full-draw-feed-off'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'set_full_draw_feed'
			enabled = $false
		})
		break
	}
	'probe-peek'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'probe_peek'
			addr = (ConvertTo-AddressValue $Address)
			len = $Length
		})
		break
	}
	'read-memory'
	{
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'read_memory'
			address = (ConvertTo-AddressValue $Address)
			size = $Length
		})
		break
	}
	'write-memory'
	{
		if ([string]::IsNullOrWhiteSpace($Bytes))
		{
			throw 'write-memory requires -Bytes as an even-length hex string.'
		}
		Assert-Success (Invoke-ResearchBridgeRequest @{
			command = 'write_memory'
			address = (ConvertTo-AddressValue $Address)
			bytes = $Bytes
		})
		break
	}
	'watch-memory'
	{
		$request = @{
			command = 'watch_memory'
			address = (ConvertTo-AddressValue $Address)
			size = $Length
			intervalMs = $WatchIntervalMs
		}
		if (-not [string]::IsNullOrWhiteSpace($Label)) { $request.label = $Label }
		Assert-Success (Invoke-ResearchBridgeRequest $request)
		break
	}
	'unwatch-memory'
	{
		$request = @{ command = 'unwatch_memory' }
		if ($All) { $request.all = $true }
		else { $request.id = $WatchId }
		Assert-Success (Invoke-ResearchBridgeRequest $request)
		break
	}
	'neck-placement'
	{
		$request = @{ command = 'set_neck_placement'; mode = $Mode }
		if ($PSBoundParameters.ContainsKey('SiteMask')) { $request.sites = $SiteMask }
		if ($FadeDry) { $request.fadeDry = $true }
		Assert-Success (Invoke-ResearchBridgeRequest $request)
		break
	}
	'highway'
	{
		$request = @{ command = 'set_highway'; mode = $Mode }
		if ($PSBoundParameters.ContainsKey('WindowSeconds'))
		{
			$request.windowSeconds = $WindowSeconds
		}
		Assert-Success (Invoke-ResearchBridgeRequest $request)
		break
	}
	'grid-gate'
	{
		$request = @{ command = 'set_grid_gate' }
		if ($PSBoundParameters.ContainsKey('Mode')) { $request.mode = $Mode }
		if ($PSBoundParameters.ContainsKey('GridSentinelValue'))
		{
			$request.sentinelValue = $GridSentinelValue
		}
		if ($PSBoundParameters.ContainsKey('GridSentinelKey'))
		{
			$request.sentinelKey = $GridSentinelKey
		}
		Assert-Success (Invoke-ResearchBridgeRequest $request)
		break
	}
}
