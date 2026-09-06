<#
.SYNOPSIS
    Drive Note by Note testing with the synthetic guitar - no cable, no playing in time.

.DESCRIPTION
    The Debug DLL installs a fake-guitar input processor on the Player 1 ASIO route. When
    armed, it replaces the captured cable with a synthesized tone at a MIDI pitch this
    script chooses, and Rocksmith's own detection runs on it exactly as on a real pickup.

    Because Note by Note freezes the transport and waits for the correct note, testing is a
    closed loop with no timing pressure: read the expected note over the research bridge,
    inject it, wait for the freeze to advance, read the next one. The 'walk' command does
    this automatically and marches through a chart hands-free, pausing at chords (which need
    a visual check) and at any note the game refuses to accept (a real finding).

    Prerequisites: a Debug host, the player in a song / Riff Repeater with Note by Note
    enabled, and Drop Pedal input shifting off (it owns the same route when on).

.EXAMPLE
    ./tools/fake-guitar-harness.ps1 status
    ./tools/fake-guitar-harness.ps1 note -Midi 40
    ./tools/fake-guitar-harness.ps1 chord -Midis 40,45,50
    ./tools/fake-guitar-harness.ps1 walk -Steps 32
#>
[CmdletBinding()]
param(
    [ValidateSet('status', 'enable', 'disable', 'note', 'chord', 'clear', 'walk', 'mode', 'cycle-mode', 'autoplay-on', 'autoplay-off', 'load-samples')]
    [string]$Command = 'status',

    # load-samples: folder of note_<midi>.wav (from tools/fretboard/export_note_bank.py).
    # Injected notes then play this guitar's real recordings instead of the synth tone.
    [string]$Folder,

    # note: the MIDI number to play.
    [int]$Midi,

    # chord: the MIDI numbers to strum together (up to six).
    [int[]]$Midis,

    # walk: how many notes to march through before stopping.
    [ValidateRange(1, 4096)]
    [int]$Steps = 16,

    [ValidateRange(0.0, 1.0)]
    [float]$Amplitude = 0.6,

    [ValidateRange(10, 8000)]
    [int]$DurationMs = 400,

    [ValidateRange(0, 4000)]
    [int]$LeadSilenceMs = 60,

    # walk: how long to wait for the freeze to accept an injected note before calling it a
    # stall (a note the detector would not accept - worth a look).
    [ValidateRange(200, 10000)]
    [int]$AcceptTimeoutMs = 2500
)

$ErrorActionPreference = 'Stop'

function Invoke-Bridge
{
    param([Parameter(Mandatory)][hashtable]$Request)

    $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.', 'RSModsPlus.Research',
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
            $line = $reader.ReadLine()
            if ([string]::IsNullOrWhiteSpace($line))
            {
                throw 'The research bridge closed the pipe without returning a response.'
            }
            $writer.WriteLine('ack')
            $response = $line | ConvertFrom-Json
            if (-not $response.ok)
            {
                throw "Bridge command failed: $($response.error)"
            }
            return $response
        }
        finally
        {
            try { $reader.Dispose() } catch [System.IO.IOException] {}
            try { $writer.Dispose() } catch [System.IO.IOException] {}
        }
    }
    finally
    {
        $pipe.Dispose()
    }
}

$NoteNames = @('C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B')
function Get-NoteName
{
    param([int]$MidiNumber)
    if ($MidiNumber -lt 0) { return '-' }
    $name = $NoteNames[$MidiNumber % 12]
    $octave = [math]::Floor($MidiNumber / 12) - 1
    return "$name$octave"
}

function Assert-HarnessReady
{
    param([object]$Status)

    if (-not $Status.fakeGuitar.installed)
    {
        throw 'The fake-guitar harness is not installed. Use a Debug host and make sure Drop Pedal input shifting is off (it owns the same ASIO route).'
    }
    if (-not $Status.fakeGuitar.captureReady)
    {
        throw 'The input route is not live yet. Get into a song (or Riff Repeater) so the cable capture attaches, then retry.'
    }
}

switch ($Command)
{
    'status'
    {
        $status = Invoke-Bridge @{ command = 'status' }
        $fg = $status.fakeGuitar
        $dp = $status.dropPedal
        [pscustomobject]@{
            Installed    = $fg.installed
            SynthEnabled = $fg.synthEnabled
            CaptureReady = $fg.captureReady
            CurrentMidi  = "$($fg.currentMidi) ($(Get-NoteName $fg.currentMidi))"
            Pending      = $fg.pending
            NbnEnabled   = $status.noteByNoteEnabled
            PitchMode    = if ($null -ne $dp) { "$($dp.pitchMode)  (input shift $($dp.inputShiftSemitones) st)" } else { '-' }
            ExpectedMidi = if ($null -ne $status.state) { "$($status.state.expectedMidi) ($(Get-NoteName $status.state.expectedMidi))" } else { '(no NBN state)' }
            ExpectedStrFrt = if ($null -ne $status.state) { "str $($status.state.selectedString) fret $($status.state.selectedFret)" } else { '-' }
        } | Format-List
        break
    }
    'mode'
    {
        $dp = (Invoke-Bridge @{ command = 'status' }).dropPedal
        if ($null -eq $dp) { throw 'No Drop Pedal state returned.' }
        [pscustomobject]@{
            ConfiguredEnabled = $dp.configuredEnabled
            PitchMode         = $dp.pitchMode
            SpeakerMode       = $dp.speakerMode
            ShiftSemitones    = $dp.shiftSemitones
            NbnInputShift     = "$($dp.inputShiftSemitones) st  (the frame NBN's expected notes are in)"
            Route             = $dp.routeName
        } | Format-List
        break
    }
    'cycle-mode'
    {
        $dp = (Invoke-Bridge @{ command = 'cycle_pitch_mode' }).dropPedal
        Write-Host ("Pitch mode is now: {0}  (input shift {1} st)" -f $dp.pitchMode, $dp.inputShiftSemitones) -ForegroundColor Cyan
        break
    }
    'autoplay-on'
    {
        Invoke-Bridge @{ command = 'set_fake_guitar_autoplay'; enabled = $true } | Out-Null
        Write-Host 'Autoplay ON. The mod now injects each frozen Note by Note target (note/chord/bend) hands-free.' -ForegroundColor Yellow
        break
    }
    'autoplay-off'
    {
        Invoke-Bridge @{ command = 'set_fake_guitar_autoplay'; enabled = $false } | Out-Null
        Write-Host 'Autoplay OFF.' -ForegroundColor Green
        break
    }
    'enable'
    {
        Invoke-Bridge @{ command = 'set_fake_guitar'; enabled = $true } | Out-Null
        Write-Host 'Fake guitar armed. The Player 1 cable is now synthetic.' -ForegroundColor Yellow
        break
    }
    'disable'
    {
        Invoke-Bridge @{ command = 'set_fake_guitar'; enabled = $false } | Out-Null
        Write-Host 'Fake guitar disarmed. The real cable passes through again.' -ForegroundColor Green
        break
    }
    'clear'
    {
        Invoke-Bridge @{ command = 'inject_clear' } | Out-Null
        Write-Host 'Inject queue cleared.'
        break
    }
    'load-samples'
    {
        # Load a note_<midi>.wav bank so injected notes are this guitar's real recordings
        # (played from memory on the audio thread - no per-note latency). Empty -Folder
        # falls back to the host default (RSModsResearch\GuitarSamples).
        $req = @{ command = 'load_guitar_samples' }
        if ($Folder) { $req.folder = $Folder }
        $resp = Invoke-Bridge $req
        $n = $resp.samplesLoaded
        if ($n -gt 0)
        {
            Write-Host "Loaded $n note samples. Injected notes now play the real guitar tone." -ForegroundColor Yellow
        }
        else
        {
            Write-Host "No note_<midi>.wav found$(if ($Folder) { " in $Folder" }). Falling back to the synth tone." -ForegroundColor Red
        }
        break
    }
    'note'
    {
        if (-not $PSBoundParameters.ContainsKey('Midi')) { throw 'note requires -Midi.' }
        Invoke-Bridge @{
            command = 'inject_note'; midi = $Midi; amplitude = $Amplitude
            durationMs = $DurationMs; leadSilenceMs = $LeadSilenceMs
        } | Out-Null
        Write-Host "Injected MIDI $Midi ($(Get-NoteName $Midi))."
        break
    }
    'chord'
    {
        if (-not $Midis -or $Midis.Count -lt 2) { throw 'chord requires -Midis with at least two MIDI numbers.' }
        Invoke-Bridge @{
            command = 'inject_chord'; midis = $Midis; amplitude = $Amplitude
            durationMs = $DurationMs; leadSilenceMs = $LeadSilenceMs
        } | Out-Null
        $names = ($Midis | ForEach-Object { Get-NoteName $_ }) -join ' '
        Write-Host "Injected chord $($Midis -join ',') ($names)."
        break
    }
    'walk'
    {
        $status = Invoke-Bridge @{ command = 'status' }
        Assert-HarnessReady $status
        if (-not $status.noteByNoteEnabled)
        {
            throw 'Note by Note is not enabled. Enable it in the song, then retry.'
        }
        Invoke-Bridge @{ command = 'set_fake_guitar'; enabled = $true } | Out-Null
        Write-Host "Walking up to $Steps notes. Ctrl+C to stop.`n" -ForegroundColor Cyan

        $accepted = 0
        for ($step = 1; $step -le $Steps; $step++)
        {
            $status = Invoke-Bridge @{ command = 'status' }
            if ($null -eq $status.state)
            {
                Write-Host "step $step  no Note by Note state (left the song?). Stopping." -ForegroundColor Red
                break
            }

            $target = [int]$status.state.expectedMidi
            $record = $status.state.selectedRecord
            $epoch = $status.state.epoch
            $chordId = [int]$status.state.selectedChordId
            $str = $status.state.selectedString
            $fret = $status.state.selectedFret

            # Emulate the player muting the previous note before picking the next one:
            # the sounding sample otherwise rings past the lifecycle's input-release
            # confirmation and the fresh onset is discarded as "carried".
            Invoke-Bridge @{ command = 'inject_clear' } | Out-Null
            Start-Sleep -Milliseconds 250

            # Decide what to play: a bend (glide to the accept pitch), a chord (all its
            # tones) or a single note.
            $isChord = $chordId -ge 0
            $bendAccept = [int]$status.state.bendAcceptMidi
            $isBend = (-not $isChord) -and $status.state.isBendTarget -and $bendAccept -gt 0 `
                -and $target -gt 0 -and $bendAccept -ne $target
            if ($isBend)
            {
                $label = "bend {0} ({1}) -> {2} ({3}) str {4} fret {5}" -f $target, (Get-NoteName $target), $bendAccept, (Get-NoteName $bendAccept), $str, $fret
                try
                {
                    # The injector's real glide path (same QueueBend the autoplay driver
                    # uses): strike the fretted pitch, rise to the accept pitch, hold.
                    Invoke-Bridge @{
                        command = 'inject_bend'; midi = $target; endMidi = $bendAccept
                        amplitude = $Amplitude
                    } | Out-Null
                }
                catch
                {
                    # Host predates inject_bend. The live host's autoplay driver already
                    # renders a true glide (QueueBend: strike, hold, ramp to the accept
                    # pitch), so pulse autoplay on for this one note and let the DLL bend
                    # it; the accept-poll below turns it back off the moment the freeze
                    # advances. Two flat injections do NOT satisfy the bend tracker - it
                    # wants the continuous rise.
                    Invoke-Bridge @{ command = 'set_fake_guitar_autoplay'; enabled = $true } | Out-Null
                    $script:AutoPlayPulse = $true
                }
            }
            elseif ($isChord)
            {
                $tones = @($status.state.expectedChordTones | Where-Object { $null -ne $_ })
                if ($tones.Count -lt 2)
                {
                    Write-Host ("step {0}  CHORD at record {1} (chordId {2}) - tones not resolved yet (need a held eval tick); visual check. Stopping." -f $step, $record, $chordId) -ForegroundColor Magenta
                    break
                }
                $label = "chord " + (($tones | ForEach-Object { Get-NoteName $_ }) -join '+')
                Invoke-Bridge @{
                    command = 'inject_chord'; midis = $tones; amplitude = $Amplitude
                    durationMs = $DurationMs; leadSilenceMs = $LeadSilenceMs
                } | Out-Null
            }
            else
            {
                if ($target -le 0)
                {
                    Write-Host ("step {0}  no single-note expected (target {1}); waiting..." -f $step, $target) -ForegroundColor DarkGray
                    Start-Sleep -Milliseconds 150
                    continue
                }
                $label = "{0} ({1}) str {2} fret {3}" -f $target, (Get-NoteName $target), $str, $fret
                Invoke-Bridge @{
                    command = 'inject_note'; midi = $target; amplitude = $Amplitude
                    durationMs = $DurationMs; leadSilenceMs = $LeadSilenceMs
                } | Out-Null
            }

            # Accept signal: the freeze advances (selectedRecord / epoch / expectedMidi move).
            # This is path-independent, so it works the same for notes and chords.
            $deadline = (Get-Date).AddMilliseconds($AcceptTimeoutMs)
            $advanced = $false
            while ((Get-Date) -lt $deadline)
            {
                Start-Sleep -Milliseconds 60
                $poll = Invoke-Bridge @{ command = 'status' }
                if ($null -eq $poll.state) { continue }
                if ($poll.state.selectedRecord -ne $record `
                    -or $poll.state.epoch -ne $epoch `
                    -or [int]$poll.state.expectedMidi -ne $target)
                {
                    $advanced = $true
                    break
                }
            }

            # End an autoplay pulse (bend fallback) as soon as the note resolves either
            # way, so autoplay never runs ahead and plays the following notes itself.
            if ($script:AutoPlayPulse)
            {
                Invoke-Bridge @{ command = 'set_fake_guitar_autoplay'; enabled = $false } | Out-Null
                $script:AutoPlayPulse = $false
            }

            if ($advanced)
            {
                $accepted++
                Write-Host ("step {0}  played {1}  -> accepted, advanced" -f $step, $label) -ForegroundColor Green
            }
            else
            {
                $why = if ($isBend) { "bend glide not confirmed by the bend tracker" } elseif ($isChord) { "synthetic strum rejected by the polyphonic matcher (tier-2 real WAV likely needed)" } else { "detector rejected the exact expected note" }
                Write-Host ("step {0}  played {1}  -> NOT accepted after {2} ms. Stopping ({3})." -f $step, $label, $AcceptTimeoutMs, $why) -ForegroundColor Red
                break
            }
        }

        Write-Host "`nWalk done. $accepted note(s) accepted and advanced." -ForegroundColor Cyan
        break
    }
}
