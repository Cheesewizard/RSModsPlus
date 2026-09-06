#pragma once

#include <cstdint>

namespace RawPitchVerifier
{
	struct RawAttackFrame
	{
		uint64_t endSampleIndex = 0;
		uint64_t attackSampleIndex = 0;
		float inputLevelDb = -160.0f;
	};

	struct RawAttackBatch
	{
		uint64_t endSampleIndex = 0;
		uint32_t sampleRate = 0;
		uint32_t count = 0;
		uint32_t reset = 0;
		RawAttackFrame frames[64] = {};
	};
}
