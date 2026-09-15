#pragma once

#include <cstdint>

namespace NoteByNote
{
	class ChordAttackGate
	{
	public:
		static bool IsFretHandMuted(uint32_t noteMask)
		{
			return (noteMask & 0x00000008u) != 0;
		}

		bool TryConsumeForNote(uint32_t noteMask, bool pitchesMatch, uint64_t now, uint32_t sampleRate)
		{
			return TryConsume(IsFretHandMuted(noteMask) || pitchesMatch, now, sampleRate);
		}

		void Clear()
		{
			attackSample = 0;
			lastObservedSample = 0;
			consumedThroughSample = 0;
		}

		void BeginHold(uint64_t sample)
		{
			attackSample = 0;
			if (sample > consumedThroughSample) consumedThroughSample = sample;
		}

		uint64_t GetAttackSample() const { return attackSample; }

		uint64_t GetConsumedThroughSample() const { return consumedThroughSample; }

		bool Observe(uint64_t sample)
		{
			if (sample <= lastObservedSample || sample <= consumedThroughSample) return false;
			lastObservedSample = sample;
			attackSample = sample;
			return true;
		}

		bool HasFreshAttack(uint64_t now, uint32_t sampleRate) const
		{
			return sampleRate != 0 && attackSample != 0 && now >= attackSample
				&& now - attackSample <= sampleRate * 3ULL / 10;
		}

		bool TryConsume(bool chordMatches, uint64_t now, uint32_t sampleRate)
		{
			if (!chordMatches || !HasFreshAttack(now, sampleRate)) return false;
			consumedThroughSample = now;
			attackSample = 0;
			return true;
		}

	private:
		uint64_t attackSample = 0;
		uint64_t lastObservedSample = 0;
		uint64_t consumedThroughSample = 0;
	};
}
