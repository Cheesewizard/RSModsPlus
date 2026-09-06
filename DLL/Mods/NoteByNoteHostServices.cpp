#include "NoteByNoteHostServices.hpp"

#include <iomanip>

#include "../Log.hpp"
#include "NoteByNoteProbe.hpp"
#include "DropPedal/DropPedal.hpp"
#include "../Audio/RawPitchVerifier.hpp"
#include "../Audio/MlAudioExporter.hpp"
#include "../Audio/MlStringFretReader.hpp"

#include <atomic>
#include <cmath>
#include <string>

namespace NoteByNoteHostServices
{
	namespace
	{
		std::atomic<LogTelemetrySink> logTelemetrySink{ nullptr };

		uint8_t __cdecl HostIsNoteByNoteEnabled()
		{
			// The probe's scoring detour reads this as its FREEZE gate, so it must be true only
			// while actually practicing a Riff Repeater loop - not in the preview menu or on other
			// screens (NBN freezes only when practicing the loop). The overlay reads the raw
			// IsAutomaticEnabled separately, so the "ON" indicator still shows in the RR menu.
			return NoteByNoteProbe::IsFreezeActive() ? 1 : 0;
		}

		int __cdecl HostGetInputOnsetShiftSemitones()
		{
			// The semitones the INPUT's detected pitch is shifted from the authored (guitar) tuning
			// frame. Only the Drop Pedal RETUNES the input, so only it shifts the detected frame; the
			// probe adds this to (authored open + fret) to get the pitch the detector will report.
			//   - Drop Pedal "E->Eb (-1)": input pitch-shifted down, detected = authored + fret + (-1).
			//     GetShiftSemitones() = -1, returned as-is.
			//   - Speaker Mode: shifts the SONG mix, NOT the input - the guitar is detected in its own
			//     authored (E) frame, so 0. (Proven live, Arpeggios Eb->E: the E guitar's
			//     fret 7 is detected as E fret 7, so expected must stay in the E frame; applying the
			//     Speaker shift put expected a semitone low and accepted the fret below.)
			//   - Off: 0.
			// The shift belongs to Drop Pedal, not Speaker Mode.
			//
			// GRADE AGAINST THE SONG, NOT THE DETUNE: the shift added here must be
			// the shift that puts the input in tune WITH THE CHART - chart reference minus the
			// player's base - not the raw dialed shift. They are equal whenever the pedal is set
			// correctly, so legitimate retunes (Eb chart + E guitar + -1) are unchanged. They
			// diverge only when the player dials the pedal to a tuning the song does not want
			// (E chart + E guitar + -1): the old code added the dialed -1 to expected too, so
			// expected chased the input down to Eb and every layer agreed on the out-of-tune
			// note. Adding the chart-match shift instead keeps expected on the song's real pitch,
			// so the extra detune surfaces as the miss it should be. Computed live from the chart
			// tuning + base, so it stays fully dynamic as the pedal or song changes. When the
			// chart tuning is not readable yet (menus, tuner populating) fall back to the dialed
			// shift - the prior behaviour - rather than force a spurious miss.
			if (DropPedal::GetPitchMode() != DropPedal::PitchMode::DropPedal) return 0;
			int chartMatchShift = 0;
			if (DropPedal::TryGetChartMatchShiftSemitones(chartMatchShift)) return chartMatchShift;
			return DropPedal::GetShiftSemitones();
		}

		void __cdecl HostHandleControllerFault(const char* reason)
		{
			NoteByNoteProbe::HandleNativeControllerFault(
				reason == nullptr ? "The research probe reported an unspecified fault" : reason);
		}

		void __cdecl HostPublishExpectedAttackEvent(
			const ResearchProtocol::ExpectedAttackEvent* source)
		{
			if (source == nullptr) return;

			NoteByNoteProbe::NativeExpectedAttackEvent event;
			event.kind = static_cast<NoteByNoteProbe::NativeExpectedAttackEventKind>(source->kind);
			event.ownerAddress = source->ownerAddress;
			event.epoch = source->epoch;
			event.updateTime = source->updateTime;
			event.isAfterUpdate = source->isAfterUpdate != 0;
			event.isNotePresent = source->isNotePresent != 0;
			event.note.nativeNoteAddress = source->note.nativeNoteAddress;
			event.note.recordAddress = source->note.recordAddress;
			event.note.noteMask = source->note.noteMask;
			event.note.noteFlags = source->note.noteFlags;
			event.note.noteHash = source->note.noteHash;
			event.note.authoredTime = source->note.authoredTime;
			event.note.nativeEventTime = source->note.nativeEventTime;
			event.note.stringIndex = source->note.stringIndex;
			event.note.fret = source->note.fret;
			event.note.chordId = source->note.chordId;
			event.note.chordNotesId = source->note.chordNotesId;
			event.note.phraseIterationId = source->note.phraseIterationId;
			event.note.rangeA4 = source->note.rangeA4;
			event.note.rangeA8 = source->note.rangeA8;
			event.note.stateC0 = source->note.stateC0;
			event.note.stateC1 = source->note.stateC1;
			event.note.stateC2 = source->note.stateC2;
			event.note.stateC3 = source->note.stateC3;
			event.note.bendSemitones = source->note.bendSemitones;
			NoteByNoteProbe::HandleNativeExpectedAttackEvent(event);
		}

		void __cdecl HostLog(ResearchProtocol::LogLevel level, const char* message)
		{
			const auto text = message == nullptr ? std::string() : std::string(message);
			switch (level)
			{
				case ResearchProtocol::LogLevel::Debug:
					Logger::GetInstance().Log(text, LogLevel::Debug);
					break;
				case ResearchProtocol::LogLevel::Info:
					Logger::GetInstance().Log(text, LogLevel::Info);
					break;
				case ResearchProtocol::LogLevel::Warning:
					Logger::GetInstance().Log(text, LogLevel::Warning);
					break;
				case ResearchProtocol::LogLevel::Error:
					Logger::GetInstance().Log(text, LogLevel::Error);
					break;
			}

			// Mirror onto the research event stream only when the debug bridge has installed a sink.
			const auto sink = logTelemetrySink.load(std::memory_order_acquire);
			if (sink != nullptr) sink(level, text.c_str());
		}

		uint8_t __cdecl HostQueryRawToneEvidence(
			double frequencyHz,
			float windowSeconds,
			ResearchProtocol::RawToneEvidence* out)
		{
			if (out == nullptr || out->structSize < sizeof(ResearchProtocol::RawToneEvidence))
			{
				return 0;
			}
			// FRAME CONTRACT (Drop Pedal double-shift fix): the caller's frequency is
			// already the OBSERVED route frame, never the player's physical frame. The probe builds
			// expectedMidi as authored + fret + HostGetInputOnsetShiftSemitones(), and that shift
			// IS the Drop Pedal's applied input shift (the only mode whose shifter is installed;
			// Speaker Mode and Off install none and return 0). The raw-pitch tap sits AFTER the
			// shifter (AsioHook: processor->Process, then RawPitchVerifier::Observe), so the route
			// audio and expectedMidi are in the same frame in every mode. Applying the applied
			// shift here a second time measured one semitone LOW under Drop Pedal E->Eb: the
			// player's correct open E (route 39) read as "+1 neighbour dominant" and was vetoed
			// (stuck open E), while a note played a semitone flat landed on the double-shifted
			// target and was accepted (Eb song + Drop Pedal). The
			// earlier multiplier was added against a Speaker-mode build whose shifter no
			// longer exists; with the applied shift reading 0 there it was a no-op until the
			// first real Drop Pedal session. Measure at the caller's frequency, no offset.
			RawPitchVerifier::ToneEvidence evidence;
			if (!RawPitchVerifier::QueryToneEvidence(frequencyHz, windowSeconds, evidence)) return 0;
			out->sampleRate = evidence.sampleRate;
			out->windowSampleCount = evidence.windowSampleCount;
			out->totalRms = evidence.totalRms;
			out->targetPower = evidence.targetPower;
			out->minusOnePower = evidence.minusOnePower;
			out->plusOnePower = evidence.plusOnePower;
			out->minusTwoPower = evidence.minusTwoPower;
			out->plusTwoPower = evidence.plusTwoPower;
			return 1;
		}

		uint8_t __cdecl HostQueryMlPitch(float* outMidi, float* outConfidence, double* outAgeSeconds)
		{
			if (outMidi == nullptr || outConfidence == nullptr || outAgeSeconds == nullptr) return 0;
			// Note the FRAME difference from tier-0: HostQueryRawToneEvidence takes a
			// physical-frame frequency and shifts it into the observed route frame itself,
			// but the ML estimate arrives already IN the observed frame (the companion hears
			// post-shifter audio). Mapping it back to expectedMidi's frame is the consumer's
			// job, using the appliedShiftSemitones the exporter stamps into the header - the
			// shadow logger deliberately logs the raw observed value.
			return MlAudioExporter::QueryResult(*outMidi, *outConfidence, *outAgeSeconds) ? 1 : 0;
		}

		uint8_t __cdecl HostIsMlPitchServiceAlive()
		{
			return MlAudioExporter::IsServiceAlive() ? 1 : 0;
		}

		// Native proposals and ML rescue share one exact-pitch verdict in the post-shifter
		// frame. A confident competing pitch can veto native acceptance; missing evidence
		// is distinct from a disagreement.
		using ResearchProtocol::MlNoteVerdict;

		MlNoteVerdict EvaluateMlNote(const MlStringFretReader::StringFret& sample,
			int expectedMidi, float minConfidence, int& observedMidi, float& confidence)
		{
			observedMidi = -1;
			confidence = 0.0f;
			if (expectedMidi < 0 || !std::isfinite(minConfidence)
				|| minConfidence <= 0.0f || minConfidence > 1.0f) return MlNoteVerdict::Unknown;
			static const int openMidi[6] = { 40, 45, 50, 55, 59, 64 };
			float targetConfidence = -1.0f;
			float otherConfidence = -1.0f;
			int otherMidi = -1;
			for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
			{
				const int fret = sample.fret[stringIndex];
				const float value = sample.conf[stringIndex];
				if (fret < 0 || fret > 19 || !std::isfinite(value) || value < 0.0f || value > 1.0f) continue;
				const int pitch = openMidi[stringIndex] + fret;
				if (pitch == expectedMidi)
				{
					if (value > targetConfidence) targetConfidence = value;
				}
				else if (value > otherConfidence)
				{
					otherConfidence = value;
					otherMidi = pitch;
				}
			}
			if (targetConfidence >= minConfidence && targetConfidence >= otherConfidence)
			{
				observedMidi = expectedMidi;
				confidence = targetConfidence;
				return MlNoteVerdict::Confirmed;
			}
			if (otherConfidence >= minConfidence && otherConfidence > targetConfidence)
			{
				observedMidi = otherMidi;
				confidence = otherConfidence;
				return MlNoteVerdict::Conflicting;
			}
			return MlNoteVerdict::Unknown;
		}

		uint64_t __cdecl HostGetMlAudioSampleIndex()
		{
			uint64_t sampleIndex = 0;
			uint32_t sampleRate = 0;
			MlAudioExporter::QueryAudioPosition(sampleIndex, sampleRate);
			return sampleIndex;
		}

		uint8_t __cdecl HostQueryMlNoteEvidence(int expectedMidi, float minConfidence,
			uint64_t minimumSampleIndex, ResearchProtocol::MlNoteEvidence* evidence)
		{
			if (evidence == nullptr) return 0;
			*evidence = {};
			MlStringFretReader::StringFret sample;
			if (!MlStringFretReader::TryGet(sample, 0.6)) return 0;
			evidence->analyzedSampleIndex = sample.analyzedSampleIndex;
			evidence->sampleRate = sample.sampleRate;
			evidence->ageSeconds = sample.ageSeconds;
			if (sample.analyzedSampleIndex <= minimumSampleIndex
				|| sample.shift != DropPedal::GetAppliedInputShiftSemitones())
			{
				evidence->verdict = MlNoteVerdict::Pending;
				return 1;
			}
			evidence->verdict = EvaluateMlNote(sample, expectedMidi, minConfidence,
				evidence->observedMidi, evidence->confidence);
			return 1;
		}

		const ResearchProtocol::HostApi hostApi =
		{
			ResearchProtocol::HOST_API_VERSION,
			sizeof(ResearchProtocol::HostApi),
			&HostIsNoteByNoteEnabled,
			&HostHandleControllerFault,
			&HostPublishExpectedAttackEvent,
			&HostLog,
			&HostGetInputOnsetShiftSemitones,
			&HostQueryRawToneEvidence,
			&HostQueryMlPitch,
			&HostIsMlPitchServiceAlive,
			&HostGetMlAudioSampleIndex,
			&HostQueryMlNoteEvidence
		};
	}

	const ResearchProtocol::HostApi& GetHostApi()
	{
		return hostApi;
	}

	void SetLogTelemetrySink(LogTelemetrySink sink)
	{
		logTelemetrySink.store(sink, std::memory_order_release);
	}
}
