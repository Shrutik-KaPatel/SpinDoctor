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

### 16:16 - <hash>
Set up the SpinDoctor_STM32 CubeIDE project for the final capstone
firmware. Brought in a previously validated LIS3DSHTR SPI driver, hit
and fixed an include-path issue along the way: a custom subfolder
only registers as a place the compiler looks for headers, not a place
the build system compiles .c files from. Library now lives in
Core/Inc and Core/Src like everything else in the project.

Implemented DMA burst-read of the LIS3DSH triggered by DRDY interrupt,
applying a burst-read and interrupt-chaining pattern validated in
earlier hardware bring-up work: one SPI transaction per sample (1
address byte + 6 data bytes, auto-increment enabled by default on
this chip), chained across HAL_SPI_TxCpltCallback ->
HAL_SPI_RxCpltCallback, triggered from HAL_GPIO_EXTI_Callback on PE0.
Confirmed running at full 400Hz, matching spec for ODR, DMA-driven
acquisition, and DRDY-triggered timing.

Still open: ping-pong double buffering. Not yet exposed as a bug
since the only consumer right now (throttled printf) finishes well
within the 2.5ms sample period, but will matter once FFT or NanoEdge
inference sits downstream and takes long enough to create real
contention with the next incoming sample.
