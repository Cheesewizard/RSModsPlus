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
			// An unresolved attack cannot borrow the next attack's pitch evidence.
			if (count != 0 && attacks[count - 1].confirmedMidi < 0) --count;
			if (count == CAPACITY) return false;
			attacks[count++] = attack;
			return true;
		}

		bool Take(int expectedMidi, PickedAttack& out)
		{
			while (count != 0 && attacks[0].confirmedMidi >= 0)
			{
				const PickedAttack attack = attacks[0];
				RemoveFirst();
				if (attack.confirmedMidi != expectedMidi) continue;
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
