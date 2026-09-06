#pragma once

#include <cstdint>
#include <string>

// Reader for the string/fret ML companion (issue #66, ML tier).
//
// The 64-bit Python companion (tools/ml-string-fret-service/) runs the TabCNN
// ONNX on the same Player-1 cable audio the game already exports for the tier-1
// pitch companion, and publishes its per-string fret read into a named mapping
// this host reads. The companion is OPTIONAL and this reader is READ-ONLY and
// non-blocking: TryGet returns false (and the overlay draws nothing) when the
// companion is not running or its newest result is stale, so the game behaves
// identically with no helper attached. The research probe uses fresh predictions
// as exact-pitch evidence for plain-note acceptance.
namespace MlStringFretReader
{
	struct StringFret
	{
		int shift;          // input-shifter semitones the companion observed
		int fret[6];        // per string (0 = low E .. 5 = high e): -1 silent, else 0..19
		int physFret[6];    // fret minus the input shift (player's physical frame)
		float conf[6];      // 0..1 softmax confidence per string
		double ageSeconds;  // maximum of publication age and analyzed audio age
		uint64_t analyzedSampleIndex;
		uint32_t sampleRate;
	};

	// Latest companion result via seqlock read. False when the companion is not
	// running or its newest result is older than maxAgeSeconds. Never blocks; opens
	// the mapping lazily and throttles retries so a missing companion costs nothing.
	bool TryGet(StringFret& out, double maxAgeSeconds = 0.5);

	// Pitch class for a sounding fret offset; shifted open strings can have negative offsets.
	std::string NoteNameForStringFret(int stringIndex, int fret);

	void Shutdown();
}
