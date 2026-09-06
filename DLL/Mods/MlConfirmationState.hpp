#pragma once

#include "NoteByNoteTypes.hpp"

namespace Research
{
	class MlConfirmationState
	{
	public:
		void Reset(uint64_t minimumSampleIndex)
		{
			this->minimumSampleIndex = minimumSampleIndex;
			lastSampleIndex = 0;
			confirmationCount = 0;
		}

		uint64_t GetMinimumSampleIndex() const
		{
			return minimumSampleIndex;
		}

		bool Observe(const ResearchProtocol::MlNoteEvidence& evidence)
		{
			if (evidence.verdict != ResearchProtocol::MlNoteVerdict::Confirmed
				|| evidence.sampleRate == 0 || evidence.analyzedSampleIndex <= minimumSampleIndex)
			{
				lastSampleIndex = evidence.analyzedSampleIndex;
				confirmationCount = 0;
				return false;
			}
			if (evidence.analyzedSampleIndex == lastSampleIndex) return confirmationCount >= 2;
			if (evidence.analyzedSampleIndex < lastSampleIndex
				|| evidence.analyzedSampleIndex - lastSampleIndex > evidence.sampleRate / 10)
			{
				confirmationCount = 0;
			}
			lastSampleIndex = evidence.analyzedSampleIndex;
			if (confirmationCount < 2) ++confirmationCount;
			return confirmationCount >= 2;
		}

	private:
		uint64_t minimumSampleIndex = 0;
		uint64_t lastSampleIndex = 0;
		uint32_t confirmationCount = 0;
	};
}
