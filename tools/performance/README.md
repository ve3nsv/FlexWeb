# Stage-one diagnostics

Server logging is enabled by default and persists across restarts. Click the
CPU chip as an admin to stop/start logging or download a segment. Opening the
station URL with `?metrics=1` additionally keeps up to one hour of two-second
samples in that tab for the Metrics button to export. Ordinary tabs send only
bounded summaries to the station while server logging is enabled.
Exports contain timings, byte counts and server load, not audio or commands.
Treat the export as machine information when sharing it.

The existing server-load sampler supplies process CPU (one core and whole
machine), resident memory and system load. Additional fields expose connected
browser count, queued WebSocket output and bytes written by currently active
connections. The latter resets when connections disappear; do not subtract
samples across a change in client count. Browser payload counters exclude
TCP/TLS/WebSocket overhead and count incoming traffic only.

Browser diagnostics record incoming audio/spectrum/status payload, visible-tab
animation-frame gap p95, long tasks where supported and JS heap where exposed.
Frame gaps measure scheduling, not the duration of the scope drawing function.
Unsupported measurements are null. Background samples are marked hidden.
Browser CPU and total process RAM require DevTools/OS profiling: record a
60-second DevTools Performance trace for each scenario and compare Main-thread
activity, rendering and GC; use the browser task manager for process memory.

On Windows run `powershell -File tools/performance/Collect-FlexWeb.ps1` for a
10-minute process CSV. A CPU value of 100 means one logical CPU fully occupied.
Specify a new OutputPath per experiment; collection appends to an existing CSV.

Compare identical settings: no client; scope only; PCM; Opus; NNR; decoder;
multiple clients; hidden tab; slow network; repeated connect/disconnect. Capture
build SHA, machine, browser, scope settings and active features for each run.
Use a dummy load or mock radio for transmit regression tests.

This stage does not implement the later resource optimizations or claim
measured savings. Hardware measurements and full Qt integration tests remain
required before treating an installer as validated.

## NR2
The noise reduction panel has NNR / NR2 method buttons with a common on/off
switch. NR2 offers Gamma/OSMS defaults and the AetherSDR spectral controls.
Only one method processes a stream. Both methods bypass FM, CW, data and TX;
SSB and AM are eligible, subject to the existing digital-decoder hold.
The initial NR2 uses AetherSDR's portable FFT fallback, not FFTW. No first-use
FFTW planning or additional runtime DLL is required. The FFT frame is 2048
samples; reported buffering delay follows the stream's sample rate. Measure
process CPU with diagnostics and compare NNR, NR2 and OFF under the same load.
The source is ported from AetherSDR commit 1199ed38; CPU savings are not assumed.

## Spectrum and bounded-pipeline update
Browsers subscribe to spectrum independently of the shared station-control
socket. The initial profile requests up to 25 updates/second and no more bins
than the viewport needs (bounded 128–4096). Downsampling uses each bin group's
peak to preserve narrow signals. Hidden tabs unsubscribe, except while the
built-in recorder is running. Existing clients retain the full source bins.
This stops delivery to that browser, not radio-side FFT acquisition.

Waterfall rows use circular storage and partial uploads; tuning shifts still
require a full update. Wrapped history is re-ordered when the pane is resized.

The Flex DSP mailbox is bounded to 200 ms / 32 blocks. Above 100 ms of queue
age it bypasses noise reduction; under sustained overload it discards oldest
queued blocks to prevent runaway latency. DSP telemetry reports queue wait,
processing time for a sampled block, and cumulative dropped blocks; processing
microseconds are not CPU percent. The generic non-Flex DSP path is unchanged.
Slow clients drop spectrum above 128 KiB of queued output; audio clients are
closed above 512 KiB and broadcast status closes clients above 1 MiB. Closing
an operator's connection invokes the transmit release handler.

Validate Chrome/Firefox/Safari for recording, wrap seams, resize and tuning;
compare payload and render metrics before/after under identical settings.

## Automatic persistent logging

Every two seconds the server writes CPU, RAM, connected-browser count, queued
WebSocket output, bytes written, spectrum drop counters and available Flex DSP
queue/processing counters, even with no browser connected. A separate worker
performs disk writes and downloads; at most eight log writes are pending. Slow
or failed storage drops measurements rather than blocking radio control.
Errors and skipped samples appear in the CPU-chip logging status.

Location: `WFWEB_CONFIG_DIR/performance` when that override is set; otherwise
`performance` under Qt's application-data directory. The adjacent
`performance.ini` stores `Enabled=true/false` (under QSettings' General group).
Files are `performance-0.jsonl` (current), then parts 1–15 newest to oldest.
Each segment is capped at 4 MiB: 64 MiB total, or seven days, whichever limit
removes data first. Expired files are pruned during writes and hourly even
when collection is disabled. An app crash may lose the final queued samples.

Download the latest segment using the CPU-chip popup, or select an older
segment. Access requires an admin session on the main web port. Guests,
listeners, operators and the separate REST port cannot download logs.

Connected browsers automatically submit summary timings, incoming payload
byte counts and JS heap where supported. These are labelled client-reported;
unsupported fields are null. They are not browser process CPU/RAM. No audio,
passwords or commands are recorded. These reports are rate-limited and only
whitelisted numeric fields are retained. No third-party service receives them.
WebSocket bytes are application writer statistics, not a NIC traffic capture.
Cumulative byte counts restart with the process; use timestamps/uptime when
calculating rates. The worker telemetry currently covers the Flex DSP path.
