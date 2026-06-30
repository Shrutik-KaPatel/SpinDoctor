
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
