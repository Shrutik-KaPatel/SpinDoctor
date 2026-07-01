
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
