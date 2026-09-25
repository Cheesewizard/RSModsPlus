#pragma once

#include <array>
#include <cstdint>
#include "DetectionFeedback.hpp"

namespace NoteByNote
{
	struct PickedAttack
	{
		double time = 0.0;
		uint64_t minimumSample = 0;
		// The unresolved attack this one ended, when that attack never had its confirmation
		// window (StrumHandoff.hpp); 0 otherwise.
		uint64_t cutShortPredecessorSample = 0;
		// Set at confirmation: this attack confirmed the note but is the successor's strum.
		bool strumBelongsToSuccessor = false;
		// A kept cut-short attack (see Push) whose full confirmation window has been evaluated
		// without confirming; Take discards it instead of waiting on it.
		bool rejected = false;
		uint64_t minimumMlSample = 0;
		uint64_t confirmedSample = 0;
		uint32_t sampleRate = 0;
		int candidateMidi = -1;
		int confirmedMidi = -1;
		DetectionFeedback feedback;
		int baselineMidi[2] = { -1, -1 };
		float baselinePower[2] = {};
		float baselineMinusPower[2] = {};
		float baselinePlusPower[2] = {};
	};

	class PickedAttackQueue
	{
	public:
		static constexpr unsigned CAPACITY = 32;
		static constexpr double MAX_AGE_SECONDS = 0.75;

		void Clear()
		{
			count = 0;
		}

		unsigned Count() const { return count; }

		PickedAttack* Latest()
		{
			return count == 0 ? nullptr : &attacks[count - 1];
		}

		PickedAttack* At(unsigned index)
		{
			return index < count ? &attacks[index] : nullptr;
		}

		unsigned Expire(double now)
		{
			unsigned expired = 0;
			while (count != 0 && now - attacks[0].time > MAX_AGE_SECONDS)
			{
				RemoveFirst();
				++expired;
			}
			return expired;
		}

		bool Push(const PickedAttack& attack)
		{
			if (count != 0 && attack.time <= attacks[count - 1].time) return false;
			// An unresolved attack that already had its confirmation window cannot borrow the next
			// attack's pitch evidence, so it is replaced. One the new attack CUT SHORT (it never had
			// the window a low note needs) is kept instead and confirmed once its window has passed:
			// fast repeated picking of a low note (~100 ms apart against a 150 ms window) otherwise
			// discarded every pick until the player paused (2026-09-23 1927.50: 18 picks, one note).
			// For a repeated note the borrowed audio is the same note, so the evidence still holds.
			if (count != 0 && attacks[count - 1].confirmedMidi < 0
				&& (attack.cutShortPredecessorSample == 0
					|| attack.cutShortPredecessorSample != attacks[count - 1].minimumSample)) --count;
			if (count == CAPACITY) RemoveFirst();
			attacks[count++] = attack;
			return true;
		}

		bool Take(int expectedMidi, PickedAttack& out)
		{
			while (count != 0 && (attacks[0].confirmedMidi >= 0 || attacks[0].rejected))
			{
				const PickedAttack attack = attacks[0];
				RemoveFirst();
				if (attack.rejected || attack.confirmedMidi != expectedMidi) continue;
				out = attack;
				return true;
			}
			return false;
		}

	private:
		std::array<PickedAttack, CAPACITY> attacks = {};
		unsigned count = 0;

		void RemoveFirst()
		{
			for (unsigned index = 1; index < count; ++index) attacks[index - 1] = attacks[index];
			--count;
		}
	};
}
