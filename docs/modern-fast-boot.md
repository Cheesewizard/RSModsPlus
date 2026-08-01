# Modern Fast Boot

Rocksmith 2014 can remain on a white screen for tens of seconds before its logo
appears when RS_ASIO is installed. Modern Fast Boot shortens that delay while
preserving the game's first hardware scan.

The feature is automatic when `RS_ASIO.dll` is loaded. It does not change song
enumeration or skip additional intro assets.

## What caused the delay

Startup tracing showed that Rocksmith synchronously requests
`Win32_PNPEntity` from WMI many times while looking for a Real Tone Cable. On a
system with hundreds of Plug and Play devices, every complete enumeration can
take more than a second. The repeated scans accounted for almost all of the
white-screen delay; reading the DLC archive list took roughly one second.

RS_ASIO supplies its virtual input later through the Windows Core Audio device
interfaces. It does not satisfy Rocksmith's earlier WMI cable search, so the
game completes its retry loop before requesting the ASIO endpoints.

## How the optimization works

RSModsPlus observes creation of the specific WMI locator and the
`ROOT\CIMV2` service used by Rocksmith. During startup it:

1. Allows the first `Win32_PNPEntity` enumeration to run normally.
2. Returns an empty enumerator for redundant startup retries.
3. Ends suppression when the game initializes the RSMods menu.
4. Forwards every later WMI request to its original implementation.

The scope is deliberately narrow: it activates only when `RS_ASIO.dll` is
loaded and only affects `Win32_PNPEntity` during the startup window. Systems
using a real Real Tone Cable retain Rocksmith's original device-discovery
behavior.

## Measured result

On the system used to investigate the delay, Rocksmith performed 20 sequential
PnP enumerations and reached RSMods menu initialization after approximately
28 seconds. With Modern Fast Boot, the first real scan took approximately 1.4
seconds and menu initialization occurred after approximately 5 seconds.

The existing RSMods **Fast Load** option is separate: it changes the intro
sequence. The inherited **Prevent PnP Crash** patch is also unrelated and
guards a different game bug.
