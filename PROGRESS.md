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
Built a full CMSIS-DSP FFT pipeline on top of the ping-pong accelerometer buffers, intended to validate that the sensor was picking up genuine mechanical signal before committing to NanoEdge for classification. Confirmed a real, independently-verifiable result: a clear peak at the fan's actual blade-pass frequency, matching separately-measured RPM.

The FFT branch introduced an intermittent reset bug that took a full session of systematic elimination to isolate, rather than guessing at fixes. Once isolated, made the call to abandon the FFT branch entirely rather than keep debugging it: NanoEdge does its own frequency-domain feature extraction internally, so the FFT output was never actually needed downstream. Reset main back to a pre-FFT commit via git bisect rather than trying to cherry-pick around the regression.

## Session 11
Hardened the pre-FFT baseline before moving into real data collection. Fixed a real, silent bug found during the FFT debugging arc: DHT11Task hanging forever with no crash, traced to a missing DWT cycle counter enable, delay_us() and DHT11's bit-timing depend on it entirely and it had been dropped in an earlier cleanup pass.

Added printfMutex to serialize printf() calls across tasks, after confirming newlib's internal formatting state isn't reentrant, two tasks calling printf near-simultaneously were producing corrupted, fused output. Added a full hard fault handler with register dump (PC, LR, PSR, R0-R3, R12, CFSR) over direct blocking UART, deliberately not printf, so a corrupted-stack fault can still get a diagnostic out. Added stack overflow detection (FreeRTOS Method 2) with a safe UART-only hook, this hook would end up being the single most important piece of instrumentation in the project several sessions later. Added reset-cause diagnostics (IWDG/brownout/power-on/pin/other) printed once at boot, also direct blocking UART by design.

## Session 12
Built the menu-driven UART capture system: send two characters (speed 1-3, class H/I/O) to select a target, send S to begin, then stream ~25 seconds of raw 3-axis buffers as CSV rows for NanoEdge Studio's live import.

Hit a serious UART DMA race condition once real capture volume started flowing: printf via DMA takes real transfer time, and the 400Hz accelerometer fill rate can produce data faster than a single row buffer can safely be reused for the next row while the previous one is still transmitting. Root-caused and fixed with a 4-way row-buffer ping-pong (a buffer being transmitted is never the same one AccelTask/CaptureTask writes into next) plus a bump to 460800 baud to reduce the wire-time bottleneck.

Captured all 9 raw speed/class combinations (156 windows each) and cleaned them with a Python script that strips any row not matching exactly 192 comma-separated integers, dropping menu text, diagnostic prints, and any malformed/fused rows rather than trying to repair them.

## Session 13
Fed the 9 cleaned CSVs into NanoEdge AI Studio as a 3-class classifier (healthy/imbalance/obstruction) and ran the benchmark search across roughly 19,000 model/preprocessing combinations. Best result: an SVM model, 99% quality index, 100% balanced accuracy on the benchmark, under 2.5KB combined RAM and flash footprint. Deployed the generated library into the STM32 project.

## Session 14
Wired a new InferenceTask into the firmware to run the deployed SVM model continuously against live sensor data. Found and fixed a real bug during integration: the capture-ready semaphore was only ever being released during an active training capture session, never during normal operation, so InferenceTask had nothing to consume outside of a capture window. Fixed by having AccelTask release the semaphore on every completed window unconditionally, with capture_active now only gating whether the diagnostic print also fires, not whether data flows at all.

Once live, testing revealed a real, explainable finding rather than a bug: the model classifies reliably at speed 2 and speed 3, but not at speed 1, weaker vibration amplitude at low RPM puts the fault classes too close together in feature space for the SVM's learned boundary. Flagged as a genuine modeling limitation to address via a healthy-class-focused recapture, not a pipeline defect.

Cleaned up repo organization around the NanoEdge integration: separated the real, integrated library (Middleware/NanoEdgeAI, committed) from the raw Studio download artifact and host-side emulator tooling (Middlewares/NanoEdgeAI, gitignored), after confirming via arm-none-eabi-nm that the compiled libneai.a has no malloc dependency, ruling out a heap-related concern before committing.

## Session 15
Chased a printf/menu corruption bug back to its actual source: _write() started the UART DMA transfer via HAL_UART_Transmit_DMA() but returned immediately, without waiting for HAL_UART_TxCpltCallback to confirm the hardware had actually finished. Since printf's internal formatting buffer is shared across every call site, a second task could start formatting into that same buffer while the DMA hardware was still physically reading bytes out of it for the previous call, producing fused, garbled menu text. Fixed by making _write() block on uartTxSemaphoreHandle a second time after starting the transfer, so it doesn't return until TxCpltCallback genuinely confirms completion.

That fix exposed a second, unrelated problem: fixing _write() meant every printf() call now held its calling task's stack frame open for the real wire transfer time (~13ms at 115200 baud) instead of returning almost instantly. Several tasks were already running stack sizes tight enough that this pushed them into genuine, silent stack overflows, caught one at a time across a long debugging session that also chased and disproved several other theories (FPU context-switch corruption, EXTI0/DRDY noise, a missing SysTick handler, a stuck DMA completion callback) before finally using the onboard ST-LINK debugger to pause the CPU mid-freeze and read the FreeRTOS call stack directly. Every freeze traced back to vApplicationStackOverflowHook, in three different tasks in turn as each prior one got fixed: WatchdogTask (128->512 words), AccelTask (128->256), DHT11Task (128->256). Also enabled configENABLE_FPU, which had been left disabled in the FreeRTOS config despite InferenceTask doing real floating-point SVM work, a genuine correctness risk even though it wasn't the actual cause of this particular freeze.

Key lesson: vApplicationStackOverflowHook fired once early in this session and was treated as a one-off, already-patched instance rather than an active, ongoing risk. Every task that overflowed afterward looked like a new, unrelated mystery instead of the same class of bug recurring in a different task. Next time a system hangs completely with no crash and no fault, check the stack overflow hook and the debugger's live call stack before spending time on other theories.

Bumped CAPTURE_WINDOW_COUNT from 156 to 350 windows per file for richer per-class training data, then recaptured all 9 speed/class combinations clean, once the fixes above made an uninterrupted 350-window capture actually possible. Verified every _clean.csv lands at exactly 350 rows with 192 fields each, zero dropped rows, before committing.

## Session 16
Brought the ESP32 gateway online from scratch: custom partition table with a dedicated `creds` NVS partition (separate from the WiFi driver's own internal `nvs` partition), holding WiFi SSID/password and the Gemini API key, generated via `nvs_partition_gen.py` and flashed independently of the application binary so credentials never touch the app build or the repo. WiFi station bring-up with a 5-attempt retry cap via FreeRTOS event groups, confirmed real IP acquisition.

Wired a new USART3 link on the STM32 (PB10/PB11, chosen over USART1 to avoid the on-board audio codec's shared pins) to send one JSON line per classification (fault class, all three class probabilities, DHT11 temperature) to the ESP32's UART2. Found a real framing bug immediately: reading arbitrary byte chunks via `uart_read_bytes()` fragments JSON lines mid-message; fixed by reading byte-by-byte and assembling complete lines up to `\n` before parsing.

Built the Apps Script backend (previously only live in Google's cloud editor, now committed to `backend/apps-script/Code.gs`): multiplexed `doGet`/`doPost` on an `action` field to support logging readings, and a simple trigger/explanation mailbox (`Control!A2`/`B2`) for an on-demand Gemini explanation feature, since streaming every classification to Gemini or Sheets directly would blow through both Gemini's tolerable call rate and Apps Script's daily execution quota. Settled on change-based logging (only log when fault_class actually changes) rather than time-based, after working out the Sheets cell-count and Apps Script quota math for continuous logging at ~150ms cadence.

Integrated the actual Gemini API call via `esp_http_client` with `esp_crt_bundle_attach` for TLS. Hit and resolved several real issues in sequence: `gemini-2.5-flash` is retired for new users (settled on `gemini-flash-lite-latest`, currently resolving to `gemini-3.1-flash-lite`); a stack overflow from running the HTTPS call directly in `app_main`'s task (moved to a dedicated 8192-byte-stack task, same pattern as the UART task); and a genuine architecture bug where `stm32_uart_task` blocking on a slow/flaky HTTPS POST let the STM32's continuous JSON stream overflow the UART driver's ring buffer mid-request, corrupting subsequent lines. Fixed by fully decoupling UART reading from network I/O via a FreeRTOS queue: `stm32_uart_task` only ever reads bytes and non-blocking-pushes a small log request; a separate `sheets_log_task` drains that queue and does the actual (occasionally flaky) HTTPS POST.

Also found and fixed a DNS race: two independent tasks (`gateway_poll_task`'s trigger polling and `sheets_log_task`'s logging) both hitting `script.google.com` concurrently were intermittently failing `getaddrinfo()`, since lwIP's resolver has a limited number of concurrent outstanding lookups per hostname. Serialized all Apps Script HTTP calls behind a single mutex to eliminate the race. Separately learned that Apps Script's web app always issues a 302 redirect, but executes the actual POST side effect (writing to the sheet) before that redirect fires, so a bare 302 already means success for log/submit actions, no need to chase the redirect at all; only `check_trigger`'s GET, which needs the response body, follows it, and only as GET, not POST, since the redirect target is a GET-only echo endpoint.

Verified the complete loop twice with genuinely different real fault states: triggered on a live "healthy" reading (Gemini: "your fan is in perfect working order..."), then again after physically obstructing the fan with paper (Gemini: "your fan is currently struggling to spin because something is physically blocking it..."), confirming the explanation is actually grounded in live sensor data rather than a canned response.

Key lesson: transient TLS/DNS failures (`-0x7280` handshake EOF, occasional `select() timeout`) recur intermittently throughout ESP32 HTTPS work regardless of what's being called (Gemini, Apps Script, or Google's own server test via curl from a laptop). Rather than chasing each one as a distinct bug, the right fix is architectural: make every polling/logging loop tolerate a failed cycle and retry on the next one, so a transient failure costs one skipped attempt rather than blocking the system.

Commit: b5a4918

## Session 17
Built the live-status path from STM32 all the way to a browser: a new `Live` tab in the spreadsheet acting as a 100-row ring buffer (`update_live`/`get_live` actions in Apps Script, oldest row deleted once past 100), fed by a new `live_update_task` on the ESP32 that pushes the latest classification independently of the existing change-based historical log. Found and fixed a real bug during testing: `data.imbalance || ''` and similar treated a legitimate `0` probability as falsy, silently writing blank cells instead of zero any time a class wasn't in play. Fixed with a small `orBlank()` helper that only substitutes on `undefined`/`null`, not falsy values.

Chased a real performance bug once the live task was running: the 2.5s target interval was actually landing at 7-15s, occasionally spiking past 20s. Root cause was `apps_script_post`/`apps_script_get` tearing down and rebuilding the `esp_http_client` handle on every single call, paying a full TCP+TLS handshake (1-4+ seconds on this chip) every time, compounded by three tasks (`live_update_task`, `sheets_log_task`, `gateway_poll_task`) now serializing behind the same mutex. Fixed by keeping one persistent client handle alive for the program's lifetime, reused across every call, with a retry-once-and-rebuild path for the case where Google's end silently closes an idle keep-alive connection between spaced-out calls. Instrumented with `esp_timer_get_time()` around the call itself before assuming a fix, rather than guessing from gaps in the Sheet, confirmed a single Apps Script invocation can still legitimately take 1-15+ seconds on its own even with a warm connection, an inherent property of Apps Script's container-per-invocation execution model, not something client-side optimization can fully remove. Settled on accepting that latency and keeping the interval at 2.5s rather than chasing sub-second timing Apps Script can't deliver. Also fixed a pre-existing bug in `gateway_poll_task` found while in this code: `cJSON_Parse(response)` was never actually called before checking `if (root)`, which would not have compiled as written. (Committed as 0e41706.)

Rebuilt the dashboard twice. First pass added the live tiles, a terminal-style scrolling feed (client-side only, no Sheets writes, so it has no row-count limit regardless of update speed), and a full analytics suite computed entirely in the browser from the live buffer: confidence-of-winning-class as a sparkline (a real leading indicator, since it can visibly decay well before the fault label itself flips), margin between the top and runner-up class, a rolling flapping/transition count, a linear-regression temperature slope, per-class temperature averages, a composite 0-100 health score blending all of the above, and a simple sensor-glitch heuristic flagging implausible temperature jumps between consecutive DHT11 reads. Uptime-since-last-fault deliberately reads from the permanent change-based history log rather than the live buffer, since the buffer only spans a few minutes at this cadence and the history log is what actually knows when the last real fault happened.

Second pass was a full visual rebuild into a single-screen HUD: Orbitron/Share Tech Mono type, cyan arc-reactor palette that shifts to amber across the entire interface the instant a live fault is detected, scanline and holo-grid overlays, a staggered boot-in sequence, and the 3D probability simplex promoted to the visual centerpiece (three fixed corners for the three fault classes, the live reading as a moving point with a fading trail of its last 12 positions, draggable orbit plus idle rotation, two counter-rotating rings for atmosphere). Health score rendered as a glowing radial gauge instead of a plain number. Explain-panel now snapshots and displays exactly which reading a Gemini request was made for, so the person waiting can see what's actually being explained rather than a generic spinner, even if the live tile has since moved on. All of this is pure front-end, no backend changes needed to support it, since it's all derived from data already flowing through `get_live`.

Key lesson: Apps Script's per-invocation latency is a platform characteristic, not a bug to be fixed away. The connection-reuse fix was real and worth doing (it removed a genuinely wasteful repeated handshake cost), but it does not and cannot make Apps Script itself respond in under a second, since every call still spins up a script execution container on Google's side. The right response to a platform-level constraint like this is to design around it (accept a multi-second live-update interval, decouple anything actually time-sensitive from the Apps Script round-trip) rather than keep tuning client code against a ceiling that isn't there.

Key lesson: added a defensive parse-and-skip guard on the dashboard for any live row that comes back malformed (missing field, unparseable timestamp), since Apps Script does not guarantee an atomic read against a concurrent ring-buffer write from the ESP32. Silently skipping a bad row and waiting for the next poll is the correct behavior here; rendering "Invalid Date" or `NaN` to the person is not.

Commit: 1637309
