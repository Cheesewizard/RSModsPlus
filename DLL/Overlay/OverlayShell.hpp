#pragma once
// The in-game Rocksmith Audio Bridge overlay (toggled with \): a sidebar of feature pages that drive the audio engine
// in-process (SharedOutput::DispatchControl) and the Note by Note settings live. Replaces the old tabbed
// "Audio Bridge" ImGui window. Design: docs/designs/in-game-overlay-shell-2026-09-23.md.

namespace Overlay
{
	void DrawShell(bool* open);
}
