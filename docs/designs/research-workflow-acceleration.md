# Research workflow acceleration

How the reverse engineering loop gets faster: fewer builds, and builds that no longer
require a game restart.

## Problem

The probe reload path already works well: probe-side capture logic iterates at probe
rebuild + `reload_probe` cost with the game running. What still forces a full game restart
is any change to the startup DLL (xinput1_3.dll), and three kinds of change land there:

1. A new hook address. Every detour is installed at startup by the hook host, so probing a
   newly discovered native function means editing the host.
2. A new ProbeApi entry. `IsValidProbeApi` requires an exact version match and every
   function pointer non-null, so growing the protocol rebuilds both sides and restarts the
   game.
3. Any experiment that only needs to read or flip memory, when no existing probe verb
   covers it.

Debugger-based shortcuts are not available: x64dbg software breakpoints trigger a fast
fail in Rocksmith and kill the process, so all instrumentation must go through the mod's
own hooks.

## Implemented: direct memory commands (2026-08-22)

Category 3 is now zero-build. The research bridge answers four new pipe commands, living
in the bridge rather than the probe so they work with no probe loaded and survive reloads:

- `read_memory { address, size }` returns up to 64 KB as lowercase hex. Reads go through
  `ReadProcessMemory` on the own process, so an unmapped or typo'd address returns an
  error instead of crashing the game.
- `write_memory { address, bytes }` writes up to 4 KB through page protection
  (`VirtualProtect` + `WriteProcessMemory` + `FlushInstructionCache`, protection
  restored), so both data flips and code patches work. The response carries the previous
  bytes; any experiment reverts by writing them back.
- `watch_memory { address, size, intervalMs, label }` arms a sampler (up to 32 watches,
  256 bytes each, 10 ms floor). Samples publish on the existing events stream as
  `memory-watch` entries only when the bytes change or readability flips, so a watch on a
  hot value does not flood the queue. Active watches are listed in `status`.
- `unwatch_memory { id }` or `{ all: true }` removes watches.

Addresses are JSON numbers or hex strings ("0x7E2880"). Client support is in
`tools/research-bridge.ps1`:

    ./tools/research-bridge.ps1 read-memory -Address 0x7E2880 -Length 64
    ./tools/research-bridge.ps1 write-memory -Address 0x12345678 -Bytes 01
    ./tools/research-bridge.ps1 watch-memory -Address 0x12345678 -Length 4 -Label gate
    ./tools/research-bridge.ps1 unwatch-memory -All

This cost one host rebuild and one restart, ever. Options considered: a probe-side
implementation (rejected: `probe_peek` already exists but dies with the probe and cannot
write) and an external `ReadProcessMemory` tool process (rejected: cannot serve future
in-process consumers, and a separate process adds attach friction for no gain).

## Implemented: the generic detour service (2026-08-22)

Category 1 is now probe-speed. Discovering an address in Ghidra costs a row in the probe's
request list plus a reload, with the game running. No host edit, no restart.

### How to use it

Add rows to `GenericHookList()` in
`DLL/Research/NoteByNoteProbe/NoteByNoteResearchProbe.cpp`:

    static const std::vector<ResearchProtocol::HookRequest> hooks =
    {
        { 0x007E2880u, 1u },   // address, slotId
        { 0x007A8B10u, 2u },
    };

Then rebuild the probe and reload with the game running:

    ./tools/research-bridge.ps1 reload

Every hit logs through the probe as a `(GENERIC HOOK)` line on the events stream
(`./tools/research-bridge.ps1 watch`), carrying the slot, the native caller
(`returnAddress`), `ecx`/`edx`, and the first stack arguments. Refine what
`ObserveGenericHook` records the same way: a probe rebuild and reload. The default list is
empty, so a fresh probe installs no generic hooks until you add one.

### What was built

The host gained a small detour service in `DLL/Mods/NoteByNoteHookHost.cpp`; the probe
requests hooks at load time. The design below is what shipped.

### Shape

- The ProbeApi grows two entries (one final version bump):
  `GetRequestedHooks(const HookRequest** requests, uint32_t* count)` called by the bridge
  after `Initialize`, and `ObserveGenericHook(uint32_t slotId, const HookContext* context)`
  called on every hit. A `HookRequest` is `{ address, slotId }`.
- For each requested address the host installs, once, a generated x86 stub:

      pushfd
      pushad
      push esp          ; HookContext: the pushad block plus flags
      push slotId
      call Dispatcher   ; __stdcall, observation only
      popad
      popfd
      jmp trampoline    ; always run the original

  `HookContext` is the saved register file. The saved esp locates the caller's stack, so
  the probe reads any argument of any convention (ecx for thiscall, ecx/edx for fastcall,
  stack for stdcall/cdecl) without the host knowing the signature.
- The dispatcher looks the slot up in a host-owned table: probe loaded and slot claimed
  means forward to `ObserveGenericHook` under the probe lock; otherwise fall through.
  Detours are never uninstalled (DetourFunction has no safe removal); an abandoned hook
  becomes a permanent pass-through, which is also what makes probe unload safe.
- On `reload_probe`, slots are remapped to the new probe's request list. An address hooked
  by a previous probe and not re-requested stays installed but inert.

### Install safety

Hook requests are queued, not installed from the pipe thread. The bridge applies pending
installs from an already-detoured seam on the game's main thread (the render preparation
detour), so the patch happens at a consistent point in the frame rather than while an
arbitrary thread may sit inside the target's first bytes. Functions that run on the audio
threads keep a residual race at install time; hooks on those are installed once and early
rather than churned.

### Protocol forward-compatibility, same bump

`IsValidProbeApi` changes policy: entries up to `HandleProbeCommand` stay required, later
entries become optional, gated by `structSize` and null-checked at call sites. The bridge
copies `min(structSize, sizeof(ProbeApi))` into a zero-initialised struct instead of
assigning whole-struct. After this, adding probe entry points never again forces a host
rebuild.

### What this buys and what it does not

After this lands, the loop for a newly discovered address is: add it to the probe's
request list, rebuild the probe, `reload_probe`, observe. No host change, no restart.

Limits: the service is observation-only. A hook that must change a return value or skip
the original (like the hit decision) still needs a typed detour in the host, but those are
rare and stable; the churn is almost entirely "log what reaches this function". Each stub
leaks a few bytes of executable pool for the process lifetime, which is irrelevant at this
scale.

## Considered and deferred: thin shim + reloadable core

The endgame is xinput1_3.dll as a permanent thin proxy (XInput exports, thunk pool,
loader) with everything else in a reloadable core DLL behind a function table. That would
make nearly every change restart-free, but it is a real project (thread teardown, D3D
hook re-entry, ImGui state) and the detour service removes most of the remaining restart
pressure. Revisit only if host churn stays high after the service lands.
