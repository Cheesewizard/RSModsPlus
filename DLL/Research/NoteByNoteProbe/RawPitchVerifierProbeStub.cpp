// Probe-only stub for the two RawPitchVerifier entry points that NoteByNoteNativeScoring.cpp (shared by the
// host and this probe) calls directly. The real implementation lives in the host xinput1_3.dll
// (DLL/Audio/RawPitchVerifier.cpp) and is fed by the host's GetBuffer audio tap; it also pulls in Aubio. The
// Note by Note research probe is a separate reloadable DLL with no audio tap of its own, so it cannot run the
// verifier - and linking the real file here would only add a second, permanently unfed copy (plus Aubio) that
// returns nothing anyway.
//
// Without these definitions the probe fails to link (unresolved CaptureSnapshot / QueryNoteConfirmation),
// which in turn trips the host's "probe output is missing" deploy guard for every research-capable config.
// Returning false is the honest, behaviour-preserving answer for a tap-less probe: the shadow paths that call
// these short-circuit exactly as they did before the calls were added, and the host (which links the real
// RawPitchVerifier.cpp, not this) is unaffected. If the probe ever needs real raw-pitch evidence, it should
// route through ResearchProbeRuntime (the host bridge), not compile its own verifier.
#include "Audio/RawPitchVerifier.hpp"
#include "Mods/ResearchProbeRuntime.hpp"

namespace RawPitchVerifier
{
	// The probe has no audio tap, but chord/dyad/mute attack confirmation (the direct
	// CaptureSnapshot callers in the shared NoteByNoteNativeScoring.cpp) must reach the host's
	// raw route ring, or every chord attack in a loaded probe reads as sustain. Route it through
	// the host bridge (HostApi::CaptureRawSnapshot, v9). QueryNoteConfirmation stays a stub: its
	// one shared caller (RawUnisonMatchesAttack) now calls ResearchProbeRuntime directly.
	bool CaptureSnapshot(AudioSnapshot& out) { return ResearchProbeRuntime::CaptureRawSnapshot(out); }
	bool QueryNoteConfirmation(double, NoteConfirmation&, uint64_t, uint64_t) { return false; }
}
