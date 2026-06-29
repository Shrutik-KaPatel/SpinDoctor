# SpinDoctor - Progress Log

Append-only. Entries are never edited after the fact. Corrections get a new entry, not a rewrite.

## Day 1 - 2026-06-29

### 06:10
Set up capstone tracking structure in this repo: firmware/, backend/,
data/, models/, this file. Existing dashboard left untouched.

Mounted STM32F407G-DISC1 (onboard LIS3DSH) on the fan's rear motor
housing via zip ties, behind the blade guard. Used a sponge as a
gap-filler so the board wouldn't flex independently against the
curved housing.

Flagged risk: sponge is compliant and sits directly in the vibration
path between housing and sensor. Could attenuate high-frequency
content or add a spurious resonance. Deferred the fix (swap for rigid
epoxy/hot-glue filler) pending a before/after FFT comparison on the
healthy fan, rather than redoing the mount on a hunch.

Derived the accelerometer axis mapping empirically. Board ended up
mounted at an orientation that didn't match the planned "short edge
down," and tracing it visually off the LIS3DSH reference diagram
wasn't reliable on a curved mount, so tilted the whole fan assembly
instead and read live X/Y/Z over UART.

- Rest baseline (fan off): X ~13900, Y ~3320, Z ~955
- Backward tilt: Y swings hardest (+~6000), Z flips sign, X drops modestly
- Left tilt: Z swings hard positive (+~7000), Y near baseline
- Right tilt: Z swings hard negative (-~9000), Y near baseline

Mapping: X = magnitude/residual (doesn't discriminate direction),
Y = pitch (forward/backward), Z = lean (left/right, cleanest signal).

Confirmed UART bridge wiring (PA2 to TTL RX, PA3 to TTL TX, common
GND, 3.3V logic already proven from prior FT232RNL use) correct and
working for raw-buffer capture.

No firmware or dataset changes pushed yet. This was an info-gathering
and mounting-validation session.
