# Reverse-engineering notes

## Summary

The Windows pointer-speed setting is not the cause. The game reads buffered relative
mouse axes through DirectInput and receives linear counts. Acceleration is introduced
later when mouse look is routed through the same nonlinear response function used for
look controls.

Addresses below are for the verified Steam executable with image base `0x00400000`.
Its SHA-256 is a reference identifier only; the fix intentionally performs no whole-file
hash check. Runtime validation is localized to PE image size and hook-site instruction
signatures so unrelated in-place edits, such as resolution hex edits, are accepted.

## Traced input path

| Address | Role |
| --- | --- |
| `0x00E8C3D0` | Initializes the DirectInput mouse device |
| `0x00E8C7B0` | Polls and sums buffered relative X/Y events |
| `0x008FEB30` | Converts counts to the game's mouse-look values |
| `0x008FF9A0` | Exports the current input-state structure |
| `0x005E0890` | Copies that structure into the player object |
| `0x005B1A30` | Gets the active look pair and optionally inverts Y |
| `0x005BCEB0` | Main player camera/look update |
| `0x0063CE50` | Applies the nonlinear response curve |

`0x008FEB30` produces exact multiples of `0.005` from raw mouse counts. Captures across
slow and fast movement confirmed that this stage is linear.

The active look pair reaches player offsets `+0x170/+0x174`. Hardware data watchpoints
showed that `0x005B1A30` reads this pair. Its saved return address led to `0x005BCEB0`,
which passes the values to `0x0063CE50` before integrating camera angles.

## Measured response

The `0x0063CE50` input/output hook produced 496 nonzero slow/fast samples. Below the
function's clamp thresholds, coefficient variation was only floating-point noise:

| Axis | Mean coefficient | Minimum | Maximum |
| --- | ---: | ---: | ---: |
| X | `54.67499594` | `54.67498657` | `54.67500174` |
| Y | `87.47999441` | `87.47998095` | `87.48000364` |

This establishes the response as `coefficient * input * abs(input)`. The function also
clips high-speed input, so both gain and maximum turn rate depend on mouse speed.

The same call provides dormant linear scale values. In the tested configuration they
were `4.04890203` horizontally and `1.49959314` vertically. The fix uses those dynamic
game-provided values, multiplied by user-configurable constants, rather than hard-coding
the observed scales.

## Runtime fix design

The game imports only `DirectInput8Create` from `dinput8.dll`, so the release DLL forwards
that export to the real Windows system DLL. It installs two signature-checked six-byte
trampolines:

- `0x008FF9A0`, to cache the mouse-specific exported look pair.
- `0x0063CE50`, to replace curved output only when its input matches that mouse pair.

All other samples, including controller look input, return the original function output.
The executable on disk is never edited.
