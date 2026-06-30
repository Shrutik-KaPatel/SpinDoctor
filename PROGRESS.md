# SpinDoctor - Progress Log

## Session 1
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

## Session 2
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

## Session 3

Implemented RPM sensing via Timer Input Capture (TIM4_CH1/PB6) reading
pulses from a Hall sensor and magnet mounted on a fan blade. Capture
worked correctly in isolation, but real-world mounting proved
fundamentally difficult on this hardware: any magnet large enough for
reliable detection introduced a real, physically significant rotor
imbalance at the blade radius, which got worse rather than better when
moved toward the hub due to limited surface area there. The imbalance
caused enough vibration to tilt the assembly during operation,
intermittently losing sensor alignment, and at higher fan speeds the
imbalance was severe enough to walk the entire base across the
surface it was sitting on.

Decision: descoped RPM sensing from the capstone given the time
budget. The accelerometer alone already provides a complete fault
signal for all three target classes (healthy, imbalance, obstruction)
via vibration FFT, RPM was intended as a secondary correlating signal,
not a load-bearing one. Working RPM implementation is preserved on a
separate branch and can be revisited if time allows; it is not part
of the integrated firmware going forward.

Updated sensor scope: accelerometer (primary) + internal die
temperature (secondary context). Moving to full sensor-fusion
integration next.

## Session 4
Attempted to add a dedicated motor-adjacent temperature sensor on PB6
(GPIO Output Open-Drain with pull-up), intended to replace the
internal die temperature sensor with something measuring closer to
real ambient/motor conditions. Hit a wiring/pin-conflict cleanup
issue first (a leftover timer configuration from an earlier, separate
sensor attempt was still claiming the pin), resolved by explicitly
disabling and reassigning it in CubeMX.

Built a UART-only diagnostic to debug presence detection without
needing a logic analyzer or multimeter: a function that samples the
data line repeatedly right after a reset pulse and prints the raw
high/low sequence as a string, a software equivalent of a logic
capture using only delay timing and printf. The trace came back flat
(no response at all), which combined with cross-checking actual
hardware on hand revealed the real issue: the originally planned
sensor was never in inventory to begin with. Pivoted to the DHT11
sensor, which was on hand and already had a proven driver pattern
from earlier sensor bring-up work.

Ported that proven DHT11 driver onto PB6, using the CubeMX-generated
pin macros so the implementation isn't hardcoded to one specific pin.
Confirmed working with stable, plausible ambient readings (~28C,
~45% RH) running continuously alongside the accelerometer.

Identified a real concurrency problem once both sensors were running
together: DHT11's read sequence blocks for 18ms+ per call, which is
incompatible with sharing a loop alongside the 400Hz accelerometer
path. This is the trigger for the next phase: retrofitting FreeRTOS
into the project (originally part of the locked architecture, not
yet implemented), giving DHT11 its own low-priority task so its
blocking behavior stops affecting time-sensitive sensor handling.

## Session 5
Retrofitted FreeRTOS into the project, originally part of the locked
architecture but deferred during initial sensor bring-up. Set the HAL
timebase source to TIM6 instead of SysTick before generating, since
FreeRTOS and HAL both want ownership of SysTick by default, a known
conflict that needs resolving before kernel generation, not after.

Split sensor handling into two priority-separated tasks, directly
motivated by the blocking issue found in the previous session:
AccelTask (higher priority) handles the DRDY-interrupt-driven
accelerometer flag check, completely non-blocking. DHT11Task (lower
priority) owns the slow, blocking DHT11 read on its own 3-second
cycle. Confirmed working: accelerometer output streams continuously
and unaffected while DHT11 reads happen in the background, exactly
the behavior a single shared loop couldn't provide.

Migrated incrementally and safely: enabled the FreeRTOS kernel first
and confirmed it booted cleanly with existing code still running
before moving any sensor logic into a task, then moved both sensors
into one task to confirm correctness, then split into two tasks last.
Each stage verified independently before adding the next.

## Session 6
Added IWDG (Independent Watchdog) as a system-level safety net. Used
LSI-clock-based timing (prescaler 32, reload 1999) for a roughly
2-second timeout window, independent from the main system clock by
design, since a watchdog that shares a clock with the thing it's
protecting against can fail right alongside it. Implemented as its
own dedicated low-priority FreeRTOS task that refreshes the watchdog
every 500ms, kept deliberately separate from AccelTask or DHT11Task
so the check reflects overall scheduler health, not just one specific
task staying alive. Scope is intentionally system-level hang
detection (a fully stuck scheduler or task forces a reset), not
per-task liveness monitoring, which would need each task reporting
its own health into shared state, more complexity than this project
needs right now.

Survived an accidental CubeMX "Reset Configuration" click mid-session
with no lost work, caught before generating code or saving, recovered
by discarding the in-memory reset and reopening the already-saved
.ioc file.
