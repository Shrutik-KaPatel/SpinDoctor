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

## Session 7
Added a shared DiagnosticsData struct and a FreeRTOS mutex
(diagnosticsMutex) protecting it, the piece from the original locked
architecture meant to give the two sensor tasks a single, safe place
to publish their latest readings for a future consumer (the
UART/ESP32 handoff task) to read as one consistent snapshot. AccelTask
and DHT11Task now lock, write their respective fields, and unlock
immediately, holding the mutex only for the few microseconds it takes
to copy a handful of values, never across something slow like printf,
which would needlessly block the other task.

Hit a real C gotcha while wiring this up: the struct definition was
initially placed in main.h's top Header comment block, which sits
above the file's #ifndef include guard. Since that section isn't
protected by the guard, any file including main.h more than once
indirectly (DHT11.h also includes main.h) got the struct defined
twice in the same compile, a "conflicting types" error. Fixed by
moving the struct into the guarded USER CODE BEGIN Includes section
instead, where it belongs.

No behavioral change yet, this is purely the safe-sharing
infrastructure landing correctly before anything actually consumes
the shared struct.

## Session 8
Replaced blocking HAL_UART_Transmit in _write() with DMA-driven
HAL_UART_Transmit_DMA, serialized across tasks using a binary
semaphore (uartTxSemaphore, initialized Available so the first printf
can proceed immediately without deadlocking). HAL_UART_TxCpltCallback
releases the semaphore once each transfer completes, preventing a
second task from starting a new DMA transfer into a buffer still
being read by a previous one.

Hit a real ordering bug on first flash: _write() was calling
osSemaphoreWait before the FreeRTOS scheduler had started, since
the early debug prints in USER CODE BEGIN 2 ran before osKernelStart.
Semaphore waits are scheduler-dependent and hang silently when called
pre-kernel, which starved WatchdogTask and triggered repeated IWDG
resets. Fixed by removing the pre-kernel debug printf calls that were
leftover from earlier diagnostic work, which was the right cleanup
anyway. DWT cycle counter enable lines kept since DHT11 depends on
DWT->CYCCNT for bit-timing.

DHT11 noted as collecting both temperature and humidity but only
temperature will appear in the eventual ESP32 payload. Humidity
fields retained in the diagnostics struct but not transmitted.

## Session 9
Implemented ping-pong double buffering on the LIS3DSH DMA burst-read
pipeline. Changed burst_rx_buf from a single 6-byte array to two
6-byte arrays, with active_buf_idx tracking which one DMA just
finished filling. TxCpltHandler always points DMA at the buffer
AccelTask is not currently reading, RxCpltHandler flips the index
and reconstructs XYZ from the freshly filled buffer. No behavioral
change visible at the terminal since the current consumer (a throttled
printf) finishes well within the 2.5ms sample window, but this is a
hard prerequisite for the FFT and NanoEdge inference steps coming
next, where processing time will meaningfully compete with the 400Hz
sample rate.

## Session 10
Added a CMSIS-DSP FFT pipeline to the accelerometer path. Copied the
CMSIS-DSP library into the project fresh (this project never had it,
unlike the earlier mini-project bring-up), reusing the exact known-good
file list from that prior debugging session instead of rediscovering it:
excluded the whole DSP Source tree by default, restored only
TransformFunctions (bitreversal, cfft, cfft_init, cfft_radix8,
rfft_fast, rfft_fast_init), CommonTables (common_tables, const_structs),
and ComplexMathFunctions (cmplx_mag). Also had to exclude the DSP
Examples and ComputeLibrary folders entirely, both pulled in
foreign Cortex-M7/M55/NEON startup files that collided with this
project's real STM32F407 startup code, a new failure mode not seen in
the mini-project.

Used arm_rfft_fast_f32 (real FFT, not complex) since accelerometer data
has no imaginary component, half the compute and half the buffer versus
the complex FFT used in the mini-project bring-up. Chose N=256 for the
capstone (versus N=512 in the mini-project) based on real, measured fan
RPM (900-1400) established earlier: 1.5625Hz bin resolution cleanly
resolves the expected 15-23Hz fundamental and 45-70Hz blade-pass
harmonic without the added latency of a larger window.

Architecture: a new dedicated FFTTask, lower priority than AccelTask so
FFT compute can never delay the time-critical 400Hz sampling path,
higher priority than DHT11Task/WatchdogTask so it still gets real CPU
time. AccelTask accumulates raw samples into a block-level ping-pong
pair of 256-sample buffers per axis (separate from the existing
single-sample ping-pong in the SPI DMA driver), releases a binary
semaphore once a window fills, and immediately swaps to the other half
so accumulation never stalls waiting on FFTTask. FFTTask blocks on that
semaphore, runs arm_rfft_fast_f32 + arm_cmplx_mag_f32 per axis on wake,
and finds the peak magnitude bin per axis.

Hit a real bug during wiring: the accumulation and semaphore-release
logic was initially pasted into the dead while(1) loop in main() (left
over from before the FreeRTOS retrofit, never actually reachable since
osKernelStart never returns), instead of into StartAccelTask itself.
Caught before it caused a silent no-op failure.

Validated fan-on vs fan-off comparison directly: fan-off FFT peaks are
scattered, low-magnitude noise (1-6Hz, ~1-20k), fan-on peaks are sharp
and stable, Y-axis at 56.25Hz with magnitude 100k-450k, X/Z at ~114Hz.
Confirmed this is real mechanical signal, not electrical/DMA artifact,
since the signal collapses entirely with the fan off. 56.25Hz / 3
blades = 18.75Hz fundamental = 1125 RPM, sits centered in the
independently measured 900-1400 RPM range, a strong cross-check that
the FFT pipeline is producing physically meaningful output.

## Session 11
Built and validated a CMSIS-DSP FFT pipeline on the accelerometer path (real
FFT, 256-point, dedicated FFTTask, full details in Session 10). Confirmed the
signal was real and physically meaningful: fan-on peaks were sharp and
stable, fan-off collapsed to noise, and the blade-pass frequency (56.25Hz)
matched independently measured RPM. Then discovered, during extended runtime
with FFT active, that the system began resetting on a roughly 2-second
cycle, tied to the IWDG watchdog firing.

Spent the majority of this session root-causing that regression through
systematic elimination rather than guessing. In order, ruled out: FFT
compute time (measured via DWT, consistently under 5ms), a stack overflow
in FFTTask (enabled FreeRTOS's Method 2 stack checking, canary+pointer
check, never fired even at 4x the original stack size), a hard fault
(added a full Cortex-M fault handler with register/PC dump, never fired),
DHT11's bit-banging driver (isolated by stubbing it out entirely, resets
persisted; separately, full driver review showed every wait loop has a
proper DWT timeout, no possible hang), printf/newlib reentrancy corruption
across tasks (real bug, fixed by adding printfMutex to serialize the full
printf call, not just the DMA transfer, but did not affect reset frequency),
LSI clock imprecision (widened the IWDG window fourfold, reset spacing
scaled proportionally rather than becoming rare, ruling this out), and
SPI/DMA/DRDY silently stalling (added a last-seen-DRDY timestamp check,
never triggered).

Physical/hardware causes were tested next, using the actual physical mount
and cabling as the constant while swapping firmware: flashed a bare
GPIO-only test program with zero peripherals, ran clean for several minutes
on the vibrating mount, ruling out the physical USB/power/NRST connection.
Flashed a standalone bare-metal accelerometer test (same LIS3DSH, same
SPI1/DMA/DRDY interrupt chain, no RTOS) on the same mount, also ran clean,
ruling out the sensor path itself. Flashed a minimal FreeRTOS+IWDG test
program with a deliberately hung task, confirmed IWDG fired correctly and
only when genuinely warranted, ruling out the watchdog/RTOS combination
itself being unreliable on this hardware.

With every individual component and the physical setup cleared, bisected
directly: checked out the last commit before any FFT-related work, clean
rebuilt, and ran the exact same reset-cause instrumentation. Zero resets
over multiple extended runs. This isolated the regression specifically to
the FFT/CMSIS-DSP integration, most likely something in the library
linkage or memory layout it introduced, since the isolation tests that
stubbed out FFT's actual logic (while still linking the library) never
resolved the resets on that branch, but removing the commit entirely did.

Decision: abandon the FFT pipeline going forward. NanoEdge AI Studio's own
documentation confirms it wants raw time-domain buffers for training, not
pre-computed FFT output, it applies its own preprocessing internally during
benchmarking. FFT was never load-bearing for the actual pipeline; it served
its purpose as a sensor/mount validation step and is preserved on its own
branch, not merged forward.

Rebuilt from the pre-FFT baseline and layered back five specific,
independent hardening fixes, each tested individually before moving to the
next: restored the DWT cycle counter enable (accidentally dropped in an
earlier session, was silently hanging the DHT11 task forever with no
crash and no output, a bug that predated FFT and had gone unnoticed);
added printfMutex to prevent print corruption across tasks; added a
reset-cause check using direct blocking UART (deliberately bypassing
printf, so it can never itself deadlock) reporting IWDG vs brownout vs
power-on vs pin reset; enabled stack overflow detection with a safe
UART-only hook; added a full hard fault handler with register dump. All
five confirmed stable together over an extended run.

Two known, low-severity issues identified late in the session, deliberately
left unfixed rather than risk further regressions this late: rare UART
output corruption when two tasks print at nearly the same instant (a torn
splice, cosmetic log corruption only, not data loss), and a single isolated
IWDG reset observed after a long clean run with no clear trigger. Attempted
a quick fix for the UART splice by adding a second wait on the UART
transmit semaphore inside the reset-cause handler; this was wrong, since
that handler uses blocking (non-DMA) transmission which never completes
the semaphore's paired release, permanently draining the token and freezing
every future printf in the system. Caught immediately via the same
stack-overflow/reset instrumentation this session had just added, reverted
before committing. Real lesson: uartTxSemaphore must only ever be
paired with DMA-mode transmission, since only the DMA completion callback
releases it, any blocking HAL_UART_Transmit call must not touch it.

Both remaining issues are cosmetic/rare enough to defer safely, confirmed
that a reset is a clean, total event (full reboot, not silent data
corruption), so a capture that gets interrupted by one will show an
obvious break in the stream and can simply be discarded and redone, not a
threat to data quality for the NanoEdge training captures coming next.
