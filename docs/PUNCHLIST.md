# Codex branch progress

Branch: `codex/punch-list`, based on `aether-look` at `311434b`.

First implementation batch (candidate; requires CI and station verification):

- **1:** TUNE power slider, in the shared TX rows used by the sidebar and GAIN.
- **9:** SSTV band buttons, editable server-saved frequencies and mode overrides,
  broadcast to connected browsers; reset to defaults. Standalone saves locally.
- **12:** SSTV in-passband SNR estimate; silence/unmeasurable signals show a dash.
- **14:** Receiving banner and live thumbnail over the SSTV gallery viewer.
- **16:** RF-KIT ATU labels/NO TUNE, drive hint, combined errors, unknown OPERATE
  state, FIFO commands ahead of polls, overall deadline and truncated-transfer
  failures. Existing dimmed antennas and read-only EXT are retained.
- **17:** Server/machine CPU badge, core count and process memory; FUNC toggle.
- **18:** Immediate Flex LEVEL readings, no periodic cached replay, browser
  attack/release and peak decay, video audio buffered to the display-sync delay.

Still pending: 2–8, 10–11, 13, 15, 19–21. Optional FFT-derived meter from 18c
is deferred until the native LEVEL timing is checked on the radio. The RF-KIT
manual's complete error-help catalog and dBFS-only meter labeling remain follow-up
work. CPU subcomponent attribution is deferred until the SDR pipeline exists.

Combined next build (local work in progress; not published or hardware-verified):

- User requests **one combined build** for the remaining features. Keep feature
  changes local, run focused checks, and push once the complete batch is ready.
- 2: guest sessions, default-deny command policy, binary transmit rejection,
  launcher/CLI configuration and session revocation implemented locally.
- 3: FT-991A rig definition, model-ID width fix and CAT simulator are local;
  full application integration and station testing remain.
- 4–8: reusable IQ FFT, clipping detection, rtl_tcp framing and an asynchronous
  remote source are local; an initial remote-source selector and spectrum
  routing are implemented but need server integration tests. A local RTL-SDR
  driver source and selector have mock-device coverage; real hardware testing
  remains. A lag estimator is tested in isolation. Other SDRs, Kiwi, complete
  status and automatic sync integration remain unfinished.
- 10–11: editable server-saved SSTV layouts/macros, received-image inset and
  streaming FSK callsign send/decode implemented locally. Layout and on-air
  interoperability still need verification.
- 13: conservative trailing-row blanking after the final confirmed line sync;
  a real noisy recording is still required to verify the reported failure.
- 21: asynchronous Windows CPU-temperature collection and Linux hwmon support;
  unavailable/stale readings remain explicitly unavailable. Windows/NUC hardware
  verification is outstanding; see CPU_TEMPERATURE.md.
- Focused local checks pass for temperature parsing, guest authentication policy,
  FSK framing/sample rates/tuning offsets, template validation/rendering helpers,
  and the existing meter/recorder timing simulation. Full application compilation
  and end-to-end guest enforcement checks are still outstanding.

The original requirements and historical batches follow unchanged.

---

# FlexWeb next batch (aether-look branch)

Status: all items below are implemented in build 56 (dev-aether-look).

The previous 15-item batch shipped in 1.0.55.

Digital modes and FM get a clean receive path: JS8, FT8 / FT4, FreeDV RADE,
and FM / NFM.

1. USB always. The JS8 and FT8 panels switch to USB only when the mode is
   already known at open (`currentMode !== 'USB'`); after a page reload that
   restores an open panel the mode is not known yet and the switch is skipped.
   Enforce USB once the mode arrives and whenever it changes while the panel /
   mode is on (JS8 and FT8 are USB on every band).
   RADE / FreeDV follow the SSB convention instead (freedv.org/getting-started:
   "On bands below 10 MHz, LSB is used for FreeDV. On 10MHz and above, USB is
   used."): LSB below 10 MHz, USB from 10 MHz up, re-checked on band changes.
   Restore the previous mode when RADE is turned off.
2. Receive DSP off while JS8, FT8 / FT4 or RADE is on: NB, NR, ANF, NNR, APF,
   WNB, and on a Flex any TNF inside the passband; plus a receive filter wide
   enough for the mode (about 3 kHz). Remember each setting when the mode starts
   and put it back when it ends (saveVfoSnapshot / restoreVfoSnapshot pattern;
   the handoff between panels must not restore in between).
   - Classic FreeDV (700D / 700E / 1600): same treatment (user confirmed).
3. FM and NFM (and DFM on a Flex): the same noise filters off while the slice
   is in FM, put back when it leaves FM. NNR currently counts FM as a voice
   mode (flexCommander::nnrModeOk) - take FM, NFM and DFM out of it. Only the
   filter settings change here, not the mode or the receive filter width.
3b. Speech processor (PROC) off in FM, NFM (DFM on a Flex) and FreeDV RADE
   and every classic FreeDV mode (700D, 700E, 1600; user confirmed): remember PROC on/off
   and its level when the mode starts, turn it off, put it back when the mode
   ends. Same remember/restore pattern as the noise filters.
4. Scope that behaves like AetherSDR's panadapter (SpectrumWidget mouse handlers):
   - Drag an empty part of the spectrum or waterfall: pan the view (the
     panadapter's centre moves, the VFO stays); click / double-click tunes.
   - Drag inside the passband: drag the VFO (the marker follows the pointer).
   - Drag a filter edge (about 8 px grab zone): move that edge alone -> filter
     low / high cut (`filt <slice> lo hi`), any width, either side.
   - Drag the frequency scale: zoom the span (bandwidth) around the pointer.
   - Wheel: tune by the step. A VFO pushed off-screen scrolls the view.
   Needs a free-pan scope on the Flex (today the scope is CENTER mode and the
   panadapter is re-centred on every tune: recenterPan), a draggable passband
   with both edges, and touch versions of the same gestures for phones.
   Keep the current behaviour for CLASSIC / non-Flex rigs, and FUNC > DRAG.
5. From the user's recording (FlexWeb_…3755kHz_LSB.mp4, 2026-10-21) - fixed in
   the tree, ship with this batch:
   - Recording files were still named wfweb_<call>_…; now FlexWeb_<call>_….
   - The video sidebar (1.0.55) is removed again at the user's request: the
     recording is the header and a full-width scope, as before.
6. Update RADE. FlexWeb's RADE (submodule resources/radae_nopy) is at 712f2f6
   (2026-03-18). The project moved to github.com/freedv/rade_c (the old repo
   now only holds a redirect); rade_c HEAD c8a3dc1 (2026-10-07) is 89 commits
   newer and contains ours. Relevant for V1 (what FreeDV uses on air):
   a2d77e8 "V1 rx: enable phase renormalization to prevent long-run drift",
   plus Windows/macOS build fixes and API clean-ups (RadeAPIUse.md: removed
   functions - adapt radeprocessor.cpp / wfweb.pro). Keep RADE V1: V2 is a
   development waveform, "on-air use is not recommended" (rade_c README).
   Point the submodule at freedv/rade_c, rebuild, run the FreeDV/RADE tests
   (freedv-test skill) and the EOO callsign round trip.
   codec2 (classic 700D/700E/1600): 1.2.0 is the latest release; Windows CI
   builds drowe67/codec2 master, macOS uses Homebrew, Ubuntu 24.04 /
   Debian 13 ship 1.2, Debian 12 ships 1.0 (distro package) - fine for 700D/
   700E/1600, nothing to do.
7. RF2K-S tuner indicator (done in the tree, ships with this batch): green
   TUNER AUTO / MANUAL, amber TUNING, red TUNER BYPASS / OFF - applet header
   chip and amp panel tile (ampTunerState in index.html).

# Following batch

Status: items 1-8 implemented for build 57.

1. RF2K-S OPERATE indicator styled like the tuner chip (user, 2026-10-21):
   in the applet header (`ae-amp-head`, index.html ~6590) the OPERATE /
   STANDBY / OFFLINE text becomes a filled chip in the `.ae-tuner-chip`
   shape - green OPERATE, amber STANDBY, red OFFLINE - so the two sit as a
   matching pair. Check the amp panel's OPERATE button / state uses the same
   colours.
2. MUTE button (user, 2026-10-21): one tap mutes the receive audio in the
   browser, a second tap brings back the previous VOL level. Today the only
   way is dragging VOL to 0 (setRxVolume, index.html ~9207). Keep the mute
   separate from rxVolume (don't save 0 to wfRxVolume), show it on the
   rxVolIndicator ("MUTED"), and light the button red while muted. Recorder
   and the CW / FT8 / JS8 decoders keep running (they tap before
   audioGainNode). Place it beside VOL only (GAIN panel and AUDIO / TX
   overlay, wherever the VOL slider is) - user confirmed, no extra button
   elsewhere.
3. Tuning STEP setting (user, 2026-10-21): today the wheel (scope and
   frequency display, scopeWheel / freqDisplay wheel) and the arrow keys
   always use tuneStep = 1000 and tuneUp/tuneDown round to a whole kHz.
   Add a STEP control like AetherSDR / SmartSDR: 10, 50, 100, 500 Hz,
   1, 5, 10 kHz, saved in localStorage (wfTuneStep). The wheel and Up/Down
   keys tune by it and snap to a multiple of it (not to a kHz).
   Shift/Ctrl stays the fast step (10x the step, at least 10 kHz). Show the
   step near the frequency (tap to cycle) and in FUNC; the aeScope wheel
   uses it too.
4. Click-to-tune lands on the nearest 1 kHz (user, 2026-10-21): a click on
   the spectrum / waterfall now rounds to 100 Hz (scopeFreqAt ~10791,
   ~10879, aeScope click ~10984), so clicking beside a signal ends up a
   couple of hundred Hz off. Round clicks (all drag styles, CLASSIC and
   AETHER) to the nearest 1 kHz. Dragging (VFO drag, pan, filter edges)
   keeps its fine resolution.
5. PTT latching like AetherSDR's MOX (user, 2026-10-21): latching already
   exists as FUNC > PTT: HOLD / LATCH (pttLatchMode, wfPttMode, 5 min safety
   release) but defaults to HOLD, so the user didn't find it. Make LATCH the
   default with the AetherSDR look on a Flex (a saved wfPttMode choice still
   wins), keep the 5 min safety release, keep the spacebar as hold-to-talk,
   and show the latched state clearly on the PTT button (red, "TX" / "MOX")
   so a tap-on is obvious.
6. S-meter out of step with the synced scope (user, 2026-10-21) - fixed in
   the tree: the 'update' push of each S-meter reading bypassed scope sync
   while the periodic 'meters' message was delayed by the audio delay, so
   the meter flipped between the live and the delayed value. Both now go
   through syncedSMeter() (scopeSyncDefer). Verify on the radio: meter,
   scope and audio move together; SYNC SCOPE OFF makes all three live.
7. Scope labelled off-frequency (user, 2026-10-21, screenshots): VFO on
   14.225 with an empty passband, the audio heard came from a signal drawn
   at ~14.1885 (36.5 kHz off on a 100 kHz span); a drag of the scope fixed
   it. The server labels the FFT from its own panCenterMHz, set when it
   sends a move; the radio does not echo every move, so after a burst of
   moves the radio can sit elsewhere with nothing to say so. Fix in the
   tree: sendPanCenter() re-sends the settled centre 400 ms after the last
   move, and a reported centre that differs from ours is logged
   ("Flex: pan centre ... reported, scope had ..."). Verify on the radio;
   if it recurs, the log line tells which side was wrong.
8. SSTV (user approved the mockup, 2026-10-21). Browser-side like FT8 / JS8,
   both builds (resources/web-shared/sstv*.mjs), no server component.
   - Panel: SSTV button in the bottom bar next to FT8; replaces the scope
     like the FT8 / JS8 panels (closeOtherModes peer, saveVfoSnapshot /
     restoreVfoSnapshot), clean receive path (noise tools off, 3 kHz
     filter). Sideband follows the band, NOT always USB (user, 2026-10-21):
     LSB below 10 MHz, USB from 10 MHz (same rule as FreeDV).
   - Band picker with the calling frequencies (user's list): 3.845 LSB,
     7.171 LSB (7.170 - 7.171), 14.230 USB (main calling frequency),
     21.340 USB, 28.680 USB.
   - RX: FM demod of the 1200-2300 Hz tone (Hilbert / quadrature
     frequency estimate at 12 kHz), VIS detection (AUTO) or a fixed mode,
     sync tracking per line, slant correction (fit of the sync pulses,
     sound-card clock error), picture drawn line by line with progress.
     Modes: Robot 36 / 72, Martin M1 / M2, Scottie S1 / S2 / DX,
     PD 90 / 120.
   - Tuning strip: narrow waterfall with 1200 sync, 1500 black, 2300 white
     markers; SIGNAL tile with lock, SNR, frequency offset.
   - Gallery: finished pictures as thumbnails (IndexedDB, per browser),
     tap to view, save (PNG with time / freq / mode in the name), delete.
   - TX: pick an image, crop / scale to the mode, text overlay (callsign,
     grid, short message), mode choice, VIS + picture synthesis
     (continuous phase), SEND with time on air, through streamDigiAudio and
     the TX lock like FT8; STOP aborts.
   - Recorder: sstvView() in recorder.js shows the picture building up.
   - Phone: controls stack under the picture.
   - Tests: node round trip encode -> decode for every mode, with noise,
     a frequency offset and a 0.1 % clock error; decode of real off-air
     recordings if available.

# Next batch

Status: items 1-6 implemented for build 59.

1. Off-screen VFO indicator like AetherSDR (user, 2026-10-21, screenshot:
   VFO 7.185 LSB, scope panned to 7.456-7.556). When the VFO (and its
   passband) is outside the scope, show a marker at that edge of the
   spectrum: an arrow pointing left / right with "A 7.185.000 LSB" (and the
   distance, e.g. "-321 kHz"), in the slice colour. Click / tap it to bring
   the scope back to the VFO (panCenter = VFO, Flex free pan; a recentre on
   other rigs). Also for VFO B / split TX when shown. Both looks (Aether and
   CLASSIC); hidden while the VFO is on screen. Recorder video shows it too
   (it draws the scope canvases, so draw it on the spectrum canvas overlay).
2. Keep the scope visible with the SSTV panel open (user, 2026-10-21).
   Today body.sstv-open hides #spectrumCanvas / #waterfallCanvas and the
   panel fills #scopeArea. Instead: the spectrum + waterfall stay on top
   (about 35 % of the scope area, the scope's normal clicks / drags / passband
   still work, so you can see and click SSTV signals near 14.230), the SSTV
   panel sits under it. Phone: a short spectrum strip over the panel. The
   recorder's SSTV view gets the scope strip as well. FT8 / JS8 keep their
   full-panel layout (user confirmed).
3. NNR off while transmitting (user, 2026-10-21): flexCommander::handleRxAudio
   runs nnrFilter on the radio's audio stream regardless of TX, so the
   ON-AIR TX LISTEN (radio monitor) goes through the noise-reduction model.
   Bypass NNR while `transmitting` (pass the audio through unchanged, keep
   the filter's state so RX resumes cleanly: reset() on unkey), and report
   NR GAIN as inactive during TX. Test: test_nnr with xmit 1 -> samples pass
   unchanged.
4. Mic streams all the time (user, 2026-10-21, Task Manager: 1.1 Mbit/s up
   with the mic on, not transmitting). The capture worklet sends every mic
   frame as raw 48 kHz 16-bit PCM (768 kbit/s + WebSocket/TLS overhead)
   whenever the mic is enabled (index.html micWorkletNode.port.onmessage ->
   sendTxAudio), and the server passes it on toward the radio
   (onWsBinaryMessage -> TX converter / DAX, or the RADE / FreeDV encoder
   when FreeDV is on). Send mic frames only while this browser is keying or
   about to: from the PTT press (before setPTT true, so nothing is clipped)
   until the release drain ends (pttReleaseTimer), while latched, and while
   intercom TALK is on. The mic VU / LVL row stays local (micVuFeed), so it
   keeps working with nothing sent. Check: FreeDV / RADE TX start, packet,
   FT8 / JS8 (their own tagged stream, unaffected), TX LISTEN OPERATOR,
   the USB pre-TX buffer (100 ms) still fills before the device starts.
5. Opus for the mic too (user, 2026-10-21): with AUDIO > QUALITY OPUS, the
   browser also encodes TX mic audio with WebCodecs AudioEncoder (Opus,
   48 kHz mono, ~32 kbit/s, 20 ms frames) and sends a new frame type; the
   server decodes it with libopus (already linked for the receive Opus
   path) back to 16-bit PCM and feeds the same TX path as 0x03 (TX lock,
   modem-tag rule, intercom forwarding, FreeDV / RADE encoder, packet
   gating all unchanged). FT8 / JS8 / SSTV / packet / TUNE modem audio stay
   PCM (data modes must not go through a speech codec). Browsers without
   AudioEncoder fall back to PCM. Check the start-of-TX latency and the
   USB pre-TX buffer, and that the ON-AIR / OPERATOR listeners still hear it.
6. RF-KIT (RF2K-S) amp panel: bring FlexWeb up to the AetherSDR rfkit-tune
   branch (user, 2026-10-21; reference ve3nsv/AetherSDR@rfkit-tune 6392afe:
   src/core/RfkitProtocol.*, src/core/RfkitConnection.*, src/gui/RfkitApplet.*,
   docs/architecture/rfkit-amplifier-design.md). FlexWeb already has the
   port from before that branch (src/rfkitprotocol.cpp, src/rfkitconnection.cpp,
   webServer::configureRfkit, FUNC > AMP panel + RF-POWER applet, tuner and
   OPERATE chips). Already as asked: the server is the only client (the browser
   never talks to the amp), 250 ms poll, one request in flight, 4 s timeout,
   64 KiB body cap, OPERATE / STANDBY, antennas 1-4, status text, error +
   reset, FWD 0-1600 W, SWR, TUNER indicator. Amp: 10.0.0.25:8080 (--rfkit).
   To do:
   a. Tune (modified restServer.py only). Port TuneApi / classifyTuneProbe /
      startTune: after connecting, probe PUT /tuner {} (2xx or 400 =
      supported; 404 / 405 / 501 = not; anything else unknown, asked again up
      to 3 times per connection). startTune sends PUT /tuner
      {"tuner_mode":"START"} only when supported and the last tuner readback
      is AUTO or MANUAL (refused while OFF, BYPASS, already tuning, unread).
      The reply is not state: re-read /tuner and /data. Publish tuneApi to the
      page; the TUNE button is hidden unless supported, enabled only in AUTO /
      MANUAL, disables itself on click until the next status (one start per
      click), reads TUNING only from the amp's readback
      (AUTO_TUNING_FROM_AUTO = tuning). The panel never keys the radio: the
      amp tunes only while a tune carrier is on the air, so show "key TUNE
      first" while the radio is not transmitting. Port the protocol tests
      (tests/rfkitprotocol_test.cpp) and a probe test against a mock amp
      answering 400 / 405.
   b. Reflected power 0-200 W bar (the server already sends `ref`), SWR on the
      amp's 1 / 1.5 / 2 / 3 / 5 / inf scale, orange from 1.5:1 and red from
      2:1 (reflected uses the same ratios converted to watts from forward);
      REF and SWR held at baseline below 1 W forward; unset fields "-", never
      0. Error as a banner across the panel, not only a tile.
   c. Web gating: operate / standby, tune, antenna select and error reset are
      refused by the server (not just hidden) unless the request comes from a
      signed-in named user (web login) and no other browser holds the TX lock
      (pttClient); the page shows why ("TX: <user>" / "sign in"). The
      read-only status stays visible to everyone. With web login off (no
      named users) the amp panel is read-only (user, 2026-10-21): the
      controls are disabled with "create a web user to control the amp"
      and the server refuses the commands.

# Batch after build 59

Status: implemented for build 60.

1. RF-KIT to AetherSDR rfkit-tune 94b0496 (user's hand-off
   rfkit-tune-handoff.md, verified on the RF2K-S, firmware G200C267; user's
   decisions 2026-10-22).
   a. Port RfkitProtocol from 94b0496: "OFF" parses as Off; TunerPill /
      tunerPillLabel / tunerPillInUse; protocol test. Server publishes the
      pill (label + in-use) with the amp status.
   b. Tuner badge (status only): TUNER ON (AUTO and AUTO_TUNING*), TUNER
      MANUAL (green); TUNER MANUAL BYPASS (MANUAL with setup BYPASS), TUNER
      BYPASS, TUNER OFF (red); "TUNER -" neutral. Replaces TUNER AUTO /
      MANUAL / TUNING wording.
   c. Header: [amp name, shortened with "..." and full name on hover] [tuner
      badge] [CONNECTED badge]. Badges are status only (not clickable). The
      OPERATE / STANDBY state moves to the OPERATE button.
   d. Sidebar gets the controls: row 1 OPERATE (green OPERATE / amber
      STANDBY), TUNE, RESET at equal widths; row 2 ANT 1-4 and EXT (EXT an
      indicator, never switched). Same gating as now (signed-in user, no
      other TX lock, read-only without web users). FUNC > AMP panel uses the
      same two rows.
   e. TUNE: shown only when the probe found support; lit (blue) when ready -
      tuner mode AUTO or MANUAL (MANUAL BYPASS included), idle, not locked;
      dark otherwise; amber while /tuner reports AUTO_TUNING*. No result
      flash: a failed tune is not reported by the amp. "Key TUNE first" hint
      stays while the radio is not keyed.
   f. RESET: red when the amp reports an error, green when clear. Error =
      non-empty trimmed /data status OR non-empty /operational-interface
      error (the manual's eight messages; any non-empty status counts).
      Banner shows that text. One click, no confirmation; POST /error/reset
      only while red. Works during TX; the amp ends in STANDBY.
   g. Antennas stay ANT 1-4 (the amp's names are not in the API).
   Dropped from the earlier plan: tapping the STANDBY badge, a green / red
   flash after a tune, showing "Adjustment ok" (not in the API), antenna
   names.

# Next batch (after build 62)

1. TUNE PWR slider in the TX applet (user, 2026-10-22): today it is only in
   the PROC... overlay (renderProcOverlay opt('TUNE PWR', 'tunePower')). Add a
   "TUNE" gain row under PWR / MIC / LVL / MON in #aeTxRows (0-100 %,
   flexDsp key tunePower, same slider style, orange like PWR), and in the
   classic GAIN panel's TX rows. Only when flexDsp.tunePower is known.
2. Guest view-only button on the login page (user chose option b,
   2026-10-22): "View only (no password)". Off by default; turned on by a
   server setting (settings file [Web] GuestView=true, a --guest-view flag,
   and a launcher checkbox). A guest gets a session like a user's, shown as
   "guest" in the top bar, and sees everything (scope, meters, audio,
   decoders) but every command that changes the radio, the amp or server
   settings is refused by the server (allow-list of read-only commands in
   handleCommand: audio on/off, codec, scope view settings, decoder panels'
   local actions), never holds the TX lock, cannot TALK on the intercom, and
   its controls are greyed with "View only". Guest sessions end when the
   setting is turned off; the lockout / rate limit applies to the guest
   button as to logins. No per-user view-only role (option a not wanted).
3. Yaesu FT-991A (user, 2026-10-22; user has the radio and will test).
   USB: CP210x dual COM (Enhanced = CAT, Standard) + USB audio codec. New
   rigs/FT-991A.rig adapted from FT-710.rig for yaesuCommander: MD codes
   (1 LSB, 2 USB, 3 CW-U, 4 FM, 5 AM, 6 RTTY-L, 7 CW-L, 8 DATA-L, 9 RTTY-U,
   A DATA-FM, B FM-N, C DATA-U, D AM-N, E C4FM), bands HF / 6 m / 2 m /
   70 cm, RM meter scales, filters / widths, default CAT baud, no CAT scope.
   Setup must point at the Enhanced COM port (say so in the launcher and
   the docs). Tests against a mock FT-991A CAT (tests/mock_serial_radio.py
   pattern) for frequency / mode / PTT / meters; then the user's radio.
4. RTL-SDR scope (user, 2026-10-22; has a dongle and an SDR T/R switch
   keyed from the radio, so the dongle is protected during TX). For radios
   without a CAT scope (FT-991A first; also older Icoms, TS-590SG).
   - Two inputs, one pipeline after the IQ:
     * built-in (default): the server opens the dongle with librtlsdr
       (the RTL-SDR Blog fork: V4 R828D + HF upconverter, works with plain
       dongles too). Windows needs the WinUSB driver once (Zadig), as for
       rtl_tcp.
     * remote rtl_tcp host:port (dongle on another machine, e.g. a Pi):
       "RTL0" header (tuner type, gain count), 5-byte commands (freq, sample
       rate, gain mode / gain index, ppm, direct sampling, AGC, bias-T),
       interleaved u8 IQ.
   - Every RTL2832U tuner: R820T / R820T2, R828D (V4), E4000, FC0012 /
     FC0013, FC2580; gain steps from the tuner's own table (reported by
     rtl_tcp / librtlsdr). HF: direct sampling (Q branch) below ~24 MHz on
     V3-style dongles, the V4's upconverter on a V4. Bias-T option.
   - Gain: AUTO (the highest gain that does not overload: watch the share of
     samples at 0 / 255 and back off on a strong signal) or a manual step.
   - Display: FFT server-side (window, average, dB) fed as scopeData through
     the existing scope path (like the Flex FFT), with the same noise-floor
     tracking that keeps the floor just above the bottom of the scope on any
     band / gain; the scope's level / range controls still apply. Spectrum,
     waterfall, passband, click / drag tuning, recordings, scope sync all
     unchanged.
   - Tuning follows the VFO with the Flex-style free pan (moves only near the
     edge); offset tune against the DC spike; span up to ~2 MHz (2.048 /
     2.4 MSPS), decimated for narrower spans; ppm correction (zero on WWV).
   - Timing: the dongle path adds ~20-60 ms; SYNC SCOPE already delays the
     scope to the audio, SYNC TRIM covers the rest. Hold the last frame while
     the radio transmits (server knows at once); protection is the user's
     switch.
   - Scope source selector: radio (CAT) / SDR / none; settings in the
     launcher (device or host:port, gain, ppm, bias-T, HF mode).
   - Build the input as a "scope source" interface so item 5 plugs in.
5. Airspy and SDRplay as scope sources (user, 2026-10-22), after item 4 on
   the same interface:
   - Local: SoapySDR (Boost licence) with its device modules - Airspy R2 /
     Mini (libairspy), Airspy HF+ (libairspyhf), SDRplay RSP1A / RSPdx /
     RSPduo (SoapySDRPlay3 over the SDRplay API v3, which is closed source:
     the user installs SDRplay's API; FlexWeb cannot bundle it). Also picks
     up HackRF, LimeSDR, and RTL again through one path.
   - Remote: SpyServer client (Airspy / Airspy HF+ / RTL on another machine,
     compressed FFT or IQ); SoapyRemote as the generic alternative.
   - Their 12-14 bit ADCs give far more range than an RTL; higher sample
     rates (up to 6-10 MSPS) are decimated to the span to keep CPU modest;
     gain models differ per device (LNA / mixer / IF gains, Airspy
     "linearity / sensitivity" presets, SDRplay IFGR / LNA state) - map them
     to AUTO + manual.
   - Windows build: SoapySDR + modules via vcpkg or the PothosSDR binaries;
     the hardest part of this item. Testing needs the user's devices.
6. KiwiSDR as a scope source (user, 2026-10-22; Kiwi on the home LAN).
   On the item-4 scope-source interface. The server connects as a
   waterfall-only Kiwi client (WebSocket ws://host:8073/<ts>/W/F; SET auth
   [password], SET zoom / start, SET maxdb / mindb, SET wf_speed, SET
   wf_comp=0), receives the Kiwi's own 1024-bin waterfall lines and feeds
   them through the existing scope path with the same noise-floor placement.
   No IQ, ~20-30 KB/s, near-zero CPU. Follows the VFO (start / zoom); spans
   snap to the Kiwi's zoom steps (30 MHz / 2^z: ~1.9 MHz, 940, 470, 235,
   117, 59 kHz...). HF only (0-30 MHz): on 6 m / 2 m / 70 cm fall back to
   the RTL-SDR or no scope. Uses one Kiwi channel; a refusal (all channels
   busy, wrong password) shows in the scope status. Hold the display while
   transmitting. Expected latency ~100-300 ms on the LAN (not measured):
   item 7 lines it up with the audio.
7. Automatic scope / S-meter / audio sync (user, 2026-10-22). Server stamps
   audio packets and scope frames on arrival and subtracts the known
   pipeline delays (dongle buffer, FFT averaging, sound-card buffer) for a
   baseline; then measures the remaining offset by cross-correlating the
   radio audio's loudness envelope with the scope's power inside the RX
   passband (and the S-meter readings with the audio) over 10-20 s at
   ~100 Hz, updating only on a clear peak (real signal, not noise). Applies
   it as the scope-sync trim automatically ("Sync: auto -85 ms" shown),
   keeps the last good value, SYNC TRIM overrides. For the SDR / Kiwi
   sources and for the Flex's own scope and S-meter.
8. SDR scope status readout: source, tuner / device, sample rate, gain,
   overload, server CPU share and network rate of the scope source, in the
   scope bar / FUNC.
9. SSTV band buttons + editable frequencies (user, 2026-10-22: the band
   drop-down beside CLOSE looks like a status label and was not found).
   - Replace #sstvBandSel with a row of band buttons in the SSTV panel's top
     bar (80 / 40 / 20 / 15 / 10 by default, plus any the user adds), the
     current band lit, its frequency and sideband in the tooltip and under
     the label on wider screens; a click tunes there (sideband by band as
     now).
   - Editable list: add / change / remove entries (band label, frequency,
     optional sideband override), e.g. 7.170 instead of 7.171, or 6 m / 2 m
     for the FT-991A. Stored on the server (settings file [SSTV]
     Frequencies, pushed in rigInfo like the CW macros) so every browser
     sees the same list; RESET restores the defaults.
   - Phone: the buttons wrap / scroll horizontally in the panel header.
10. SSTV templates (user, 2026-10-22): CQ / Reply / 73 picture templates
    with QSSTV-style fields - %m my call, %l locator, %f / %n my name,
    %c their call, %o their name, %r RSV report (595 best), %t time,
    %d date, %x / %y / %z comments - filled at send time; %m / %l from the
    station settings, %c from the last received FSK ID (or typed), %r typed
    with RSV steps. Pick a template per send; templates editable (text,
    position, size, colour) and kept on the server with the SSTV settings.
    Reply / QSL layout from the user's decode (2026-10-22, KE5RS / KF0YCR):
    a header bar with boxes ("TNX" | "<their call> . <RSV>" | "73"), the
    picture received from them as an inset on the left (proof of copy; a
    field like %i = the selected / last gallery picture, cropped to fit),
    and a text column on the right: "QSL", their call, their locator,
    "RSV 595", a large "73". Built-in templates: CQ (own picture + "CQ de
    %m %l"), Reply/QSL (as above), 73.
11. SSTV FSK ID (MMSSTV / QSSTV / PySSTV format: 1900 Hz = 1, 2100 Hz = 0,
    22 ms per bit, 6-bit characters (ASCII - 0x20), framed 0x20 0x2A ...
    0x01) after the picture: decode it (gallery entry and the RX status get
    the sender's call, %c filled), and send ours after every picture
    (option, on by default). Tests: encode / decode round trip and a PySSTV
    add_fskid_text WAV.
12. SSTV SNR fix (user's screenshot showed "SNR -157 dB"): the estimate uses
    2500-2800 Hz as the noise reference, which a 2.7 kHz receive filter
    removes. Measure the noise inside the passband instead (e.g. the bins
    between the picture tones / a low percentile of 1100-2300 Hz over time,
    or the sync-pulse tone's spread), clamp to a sensible range, and show
    "-" when it cannot be estimated.
13. SSTV: noise on the last lines of a picture (user's Scottie S1 from K3DCR,
    14.230, 2026-10-22: the bottom rows are streaked). Analog SSTV has no
    end marker - the mode's line count (from the VIS) ends the picture - so
    find which of these it is: the sender stopped / faded before the last
    lines; the FSK ID or the next transmission landing in the final line;
    or our timing of the last line(s) (sync search window / fit at the end).
    Fix: stop at the last line found with a sync and signal, leave lines
    with no signal blank (black) instead of noise, trim a trailing FSK ID.
    Ask the user for a REC recording of a picture that shows it.
14. SSTV "Receiving" banner (user, 2026-10-22: a picture arrived while a
    saved one was open full size and seemed not to decode). While the
    gallery viewer (#sstvView) is open and a new picture starts, show a
    banner across the viewer - "Receiving Robot 36... line 12 / 240" with a
    small live thumbnail - updating with progress; a tap closes the viewer
    and shows the live picture. When the picture completes, the banner
    reads "New picture received - tap to view". The viewer is not closed
    automatically.
15. LOW PRIORITY - NetLogger NETS panel (user, 2026-10-22). Reference:
    netlogger-mcp 0.2.0 (github.com/qso-graph/netlogger-mcp, GPL-3.0, KI7MT),
    an MCP wrapper over NetLogger's public XML Data Service API 1.3
    (https://www.netlogger.org/api/<Routine>.php: GetActiveNets,
    GetCheckins ServerName+NetName, GetPastNets, GetPastNetCheckins
    +NetID; no key). FlexWeb's server is the only client: limits per minute
    GetActiveNets 1 / GetCheckins 3 / GetPastNets 1 / GetPastNetCheckins
    10, checked before sending; cache 60 s / 20 s / 60 s / 1 h; on HTTP 429
    stop all calls >= 60 s (Retry-After honoured, max 1 h); 15 s timeout;
    body cap; strict XML parsing; User-Agent "FlexWeb/<ver> (<callsign>;
    +https://github.com/ve3nsv/FlexWeb)"; >7 days of past nets needs a name
    filter. Page: active nets (filter band / mode / name; tap to tune, mode,
    FM offset / tone when the text gives them), "net on this frequency"
    banner with the live check-in list and the pointer (station being
    worked), own call highlighted when logged; past nets browser; optional
    logbook entry for a net check-in. Read-only: NetLogger has no check-in
    submission through this service.
16. RF-KIT to AetherSDR rfkit-tune 53e95571 (user's hand-off v2,
    2026-10-23; supersedes the first hand-off; build 62 has 94b0496).
    Re-port RfkitProtocol / RfkitConnection / protocol test, then:
    a. Badge wording KPA1500-style "ATU" (replaces TUNER ON / MANUAL ...):
       ATU IN (AUTO, AUTO_TUNING*; green), ATU MAN (MANUAL + CL / LC;
       green), ATU MAN BYP (MANUAL + BYPASS; red), ATU BYP (BYPASS; red),
       ATU OFF (OFF; red), ATU NO TUNE (AUTO or MANUAL with setup NOT TUNED;
       red), "ATU -" neutral. BYPASS / OFF win over NOT TUNED; a tune in
       progress stays ATU IN. Info line "ATU <mode> <setup> L .. C ..".
    b. TUNE allowed in AUTO and MANUAL incl. MANUAL+BYPASS and NOT TUNED;
       not in BYPASS / OFF.
    c. Drive hint: outline TUNE in blue while /power forward is 4-39 W (the
       manual's tune drive range, 7.2); tooltip says 10 W or more is
       recommended below 10 W. Hint only, TUNE stays clickable.
    d. Fault banner "Error: <text>" = /data status plus the interface error
       when different; the plain STATUS tile goes. RESET as now (red while a
       fault shows, green when clear, one click, sent only while red).
    e. Connection fixes: commands ahead of queued polls but in the order
       issued (two antenna clicks arrive in order); a 10 s total deadline per
       request besides the 4 s inactivity timeout; a transfer broken after
       the headers is a transport failure (4xx / 5xx still are not).
    f. OPERATE disabled and "-" until /operate-mode has answered. A STANDBY
       can appear by itself (the amp's sleep timer).
    g. Antennas the amp does not list or that are disabled on this band are
       shown dimmed, not hidden (enabled antennas vary per band). EXT stays
       read-only (port 1-16 when reported).
    h. Keep from the manual for tooltips / help: "Not Tuned!" blocks TX with
       the tuner in line and no stored tuning (STANDBY, AUTO, carrier, Tune);
       the eight error messages and their causes; Reset leaves STANDBY.
    The user confirmed the ATU wording on 2026-10-23 (replaces TUNER ON).
17. Server CPU on the page (user, 2026-10-23). The server measures, every
    2 s: FlexWeb's own process CPU (Linux / macOS getrusage or
    /proc/self/stat, Windows GetProcessTimes; as % of one core and of the
    whole machine) and the machine's total CPU (Linux /proc/stat, macOS
    host_statistics, Windows GetSystemTimes), plus memory in use. Sent as
    {type:'serverLoad'}; the page shows a small chip in the top bar
    ("CPU 12%", amber above 70 %, red above 90 %) with a tooltip: FlexWeb vs
    whole machine, cores, memory; and, where cheap to count, the heavy parts
    (NNR, Opus encode, SDR scope once item 4 exists). Optional: hide the
    chip (FUNC). Easy.
18. S-meter still lags (user, 2026-10-23; medium). Measured from the user's
    recording (3.755 LSB): the spectrum is ~170 ms behind the recorded audio
    and the meter another ~130-200 ms behind the spectrum, ~330 ms in all.
    Two causes:
    a. Recording only: recorder.js records RX audio as it arrives, but the
       canvases it copies are delayed by scope sync (jitter buffer + output
       latency). Fix: delay the recorded audio by the same scope-sync delay,
       so the video matches what was heard.
    b. Real meter lag (~150 ms on the page): the Flex LEVEL meter's own
       averaging and rate, the server's 100 ms meter flush hold, and the
       200 ms periodic 'meters' message that re-sends the cached S value.
       Fix the AetherSDR way (user chose this, 2026-10-23; SMeterWidget,
       MainWindow_ReceiveSync): send each LEVEL reading at once (no flush
       hold), stop re-sending a stale cached sMeter, apply it through the
       same scope-sync delay as the spectrum / waterfall, and animate the
       needle / bar locally - attack ~45 ms, release ~180 ms, redrawn every
       frame; peak marker falls 0.5 dB per 50 ms, reset every 10 s. The
       radio's LEVEL stays the only source (dBFS-only radios: relative,
       marked uncalibrated).
    c. Optional, only if b is not enough on the radio: a fast S-meter from
       the panadapter FFT power in the passband, calibrated continuously to
       LEVEL (FUNC > METER > FAST). AetherSDR does not do this.
    Item 7 (auto sync check) measures the result.
19. CAT control / AetherSDR-style panadapter for all radio models (user,
    2026-10-23; hard, large). Today 16 isFlexRig gates in index.html keep
    the Aether scope (drag / free pan, off-screen VFO tab, filter-edge drag),
    NNR and some labels to the Flex. Replace them with per-rig capabilities:
    a. Icom scope: drag pans by retuning in centre mode or moves fixed edges
       (CI-V 27 1E) in fixed mode; zoom maps to the span list; recenter
       policy as AetherSDR recenter-policy.md.
    b. Filter-edge drag maps to filter width / PBT / IF shift where the rig
       has them (Kenwood, Yaesu SH/IS commands).
    c. Off-screen VFO tab and click-to-tune nearest 1 kHz on every rig.
    d. NNR moved from flexCommander into the generic webServer RX audio
       path, so every radio gets it.
    e. Radios without a scope (FT-991A etc.) use the SDR scope sources
       (items 3-5, RTL / Airspy / SDRplay / KiwiSDR) with the same UI.
    f. Controls shown per capability, from the rig files (as AetherSDR
       icom-capability-profiles.md / radio-capabilities-map.md).
    Test on the virtual rig and the user's FT-991A; Icom needs a tester.
20. All amplifiers AetherSDR supports (user, 2026-10-23; medium each).
    Port like RF-KIT (protocol + connection classes, server-side gating
    ampControlAllowed, status-only badges, OPERATE / TUNE / RESET, antenna
    row, FUNC > AMP), one --amp flag per type:
    - Acom 600S (AcomConnection / AcomProtocol)
    - Elecraft KPA500 (+ fault history, maintenance), KAT500 tuner,
      KPA1500, KXPA100
    - SPE Expert 1.3K / 1.5K / 2K, new and legacy protocol (LCD scheduler)
    - FlexRadio PowerGenius XL (PGXL) and TunerGenius XL (TGXL)
    - Vkamp
    - LP-100A wattmeter (meter only)
    Source: AetherSDR src/core/*Connection / *Protocol and
    docs/architecture/*-design.md. Unit tests per protocol and a stub amp
    per type, as tests/test_rfkit.py. Order by what users own (the user
    has the RF2K-S, already done).

21. Server CPU temperature (user, 2026-10-26; Windows first).
    - Show the FlexWeb server computer's CPU package temperature in degrees C
      beside the existing CPU/load display, with the sensor name in its tooltip.
      This is separate from the Flex radio and RF-KIT amplifier temperatures.
    - Use a supported sensor source on each platform; document any required
      Windows sensor helper/provider. Do not assume every machine exposes one.
    - Refresh alongside server-load updates without blocking audio or control.
      Report unavailable or stale sensor readings as unavailable, never 0 C.
    - Include the user's Intel Skull Canyon NUC in station testing.
