#pragma once

#include <cstdint>
#include <string>

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
	bool TryGet(StringFret& out, double maxAgeSeconds = 0.5, const char** failureReason = nullptr);

	// Pitch class for a sounding fret offset; shifted open strings can have negative offsets.
	std::string NoteNameForStringFret(int stringIndex, int fret);

	void Shutdown();
}
