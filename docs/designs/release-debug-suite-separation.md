# Release prep: separate the debug/research suite; a standalone control panel; a future VST insert

Status: planned (2026-08-31). Release task, not started.

## Goal

The release build should be the shipped mod and nothing else. All the research scaffolding
that grew during the Note by Note work (the research bridge, the fake-guitar injector, the
reloadable probes, the harness scripts) should stay in this repo but be **optional and
compiled out of release**. Two reasons:

1. A future merge with RSMods upstream should not drag the debug tooling along. The tooling
   lives here; upstream stays clean.
2. The final release should simplify to a small set of files instead of the current spread of
   probes, bridges, and harnesses.

This doc records the plan and a few design calls, and parks a larger idea (a VST insert for
custom guitar tone) that the research has now made feasible.

## Where things stand today

The debug/research pieces already exist as fairly well-bounded units:

- `DLL/Research/` - the research bridge (named-pipe JSON server) and the probe projects.
- `DLL/Mods/FakeGuitar/` - the synthetic/sample guitar injector.
- `DLL/Research/NoteByNoteProbe/` - the reloadable scoring probe (its own vcxproj).
- `tools/` - the PowerShell harness, the sample tooling, the accuracy sweeps.

Some of this is already behind `#if defined(_DEBUG)` (the `ModManager` poll calls, for
example), but the gating is scattered and tied to the Debug configuration rather than to an
explicit "research tools" switch.

## Task A: put the research tooling behind one build boundary

**Recommendation: gate research tooling on a dedicated preprocessor define, not on `_DEBUG`.**

Introduce a single macro, e.g. `RSMODS_RESEARCH`, defined by the Debug configuration (and any
future "power-user" config) and undefined for Release. Everything research-only compiles under
`#if defined(RSMODS_RESEARCH)`:

- the research bridge server and all its commands,
- the fake-guitar injector and its source-stage install,
- the probe load/poll path in `ModManager`,
- any research-only fields the shipped structs carry.

Why a dedicated macro instead of reusing `_DEBUG`:

- It decouples "is this a debug build" from "does this build carry research tools." Today they
  are the same, but a dedicated switch lets a normal Debug build of the *player* mod exist
  without the bridge, and lets a research build be produced deliberately.
- It reads as intent at every call site. A reviewer (or an upstream merge) sees exactly what
  is tooling and what is product.

**Folder isolation.** Keep all research-only code inside `DLL/Research/` and
`DLL/Mods/FakeGuitar/`. The shipped mods should not `#include` research headers except behind
the macro. That way the release build drops whole translation units, and an upstream merge can
exclude two folders cleanly.

**Probes stay debug-only.** The reloadable probe DLLs (`NoteByNoteProbe`) are a research
mechanism, not a shipped artifact. Release should not build or deploy them; the shipped Note
by Note logic is whatever has been promoted into the host. This is the point of the
host/probe split: the probe is the iteration surface, the host is the product.

**Acceptance criteria for Task A:**

- A Release build produces `xinput1_3.dll` with no research bridge, no fake guitar, no probe
  loader, and no named pipe. Verified by: pipe does not open, `strings` shows no research
  command names.
- A Debug (research) build behaves exactly as it does today.
- Removing `DLL/Research/` and `DLL/Mods/FakeGuitar/` from the tree still lets the mod compile
  in Release (proves nothing shipped depends on them).

## Task B: a standalone control panel (WinForms bridge client)

A separate Windows Forms `.exe` (its own csproj, optional download) that talks to the mod over
the **existing research bridge**. It is a bridge *client*, so it adds nothing to the mod DLL
and ships or not independently.

**Recommendation: the panel owns no mod state; it only sends bridge commands and renders
status.** Everything it does is already a bridge verb (enable/disable fake guitar, arm
autoplay, cycle pitch mode, inject, read status). New buttons map to new verbs. This keeps the
mod as the single source of truth and the panel thin.

MVP surface:

- Toggle fake guitar / autoplay, cycle Speaker Mode and Drop Pedal, show current mode and
  shift.
- Live status: game state, NBN target (note, string/fret), samples loaded, capture ready.
- A log tail of the throttled probe log.

**Later: arrow-key section navigation.** Move between notes/phrases with the arrow keys to jump
straight to a section under test, instead of playing up to it. This should drive the existing
authored phrase-section grid (owner+0x78, the stable grid the timeline controller already
uses), not a new position mechanism. A bridge verb like `seek_section {+1|-1}` or
`seek_note {+1|-1}` moves the Riff Repeater loop/target along that grid; the panel binds the
arrow keys to it. Reuses proven addressing rather than inventing a seek path.

**Why WinForms is fine here:** it is a local dev/test tool, not a shipped UI. The existing
RSMods GUI is WinForms already, so the toolchain and any shared helpers are on hand.

## Future mod idea: VST insert for custom guitar tone

Rocksmith only does note detection on the cable input; it never gives the player a good
monitored tone. A mod that runs the guitar signal through a player-supplied VST and lets them
**hear** themselves through it would modernise the game's sound considerably. The research done
for Note by Note (the ASIO capture tap, the source-before-processor chain, in-place buffer
editing, and the raw-cable WASAPI tap feasibility) means the hard parts of *getting at the
signal* are already solved. This was not obviously feasible before; it is now.

**Sketch:**

- The capture buffer is already tapped and already run through an in-place DSP slot (Drop
  Pedal). A VST insert is "the same slot, running a hosted VST instead of the pitch shifter."
- Monitoring is the real work. The game outputs the backing track, not the player's tone, so
  the mod needs its own low-latency output path that mixes the VST-processed guitar in (its
  own ASIO/WASAPI-shared render stream, or an insert into the game's output mix).
- Hosting: embedding a full VST3 host (SDK, plugin scan, editor window) in the mod is heavy.
  Consider an MVP that hosts a single headless VST3 with no GUI first, or routes to a small
  external host, before committing to in-process GUI hosting.

**Headline challenge: latency.** End-to-end must be ASIO with small buffers; every added stage
(tap, VST process, monitor render) adds delay, and guitarists notice low tens of milliseconds.
A latency budget and a measurement harness come first; if the budget does not close, the
feature does not ship regardless of how clean the routing is.

**Debug value:** the same routing/monitoring path is useful for research (hearing exactly what
the detector hears, auditioning the fake-guitar and shifter stages), so an early spike pays off
even before it becomes a player feature.

This is R&D, not release-blocking. It gets its own design doc and a latency spike before any
commitment. It is recorded here so the routing groundwork laid for the debug suite is built
with this direction in mind. Related: `docs/designs/raw-cable-input-tap.md`, issue #18.

## Open decisions for Philip

- Macro name and configuration layout: `RSMODS_RESEARCH` on Debug only, or a separate
  "Debug+Research" configuration so a plain Debug player build can exist too?
- Does the fake guitar stay purely research, or does a trimmed version become a shipped
  practice aid? (It is currently research-only.)
- Control panel as part of this repo's solution, or a sibling repo that depends only on the
  bridge protocol? (In-repo is simpler; sibling keeps upstream merges even cleaner.)
- Track these as cards on the Projects board (project #3) once scoped.

## Order of work

1. Task A (build boundary) first. It is the enabling step for a clean upstream merge and makes
   the release goal real. Low risk, mostly mechanical, high payoff.
2. Task B MVP (panel + existing verbs), then arrow-key section nav on top of the phrase grid.
3. VST insert as a separate spike, latency budget first.
