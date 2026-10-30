# Stage-one diagnostics

Open the station URL with `?metrics=1`. The Metrics button exports up to one
hour of two-second samples. Diagnostics are off on ordinary page loads.
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
