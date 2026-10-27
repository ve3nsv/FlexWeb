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

Build 68 (2026-10-27): items 27, 28, 29, 30, 31, 22's terminal names and
the FT-991A split are done (the split needs the user's test on the radio).

Build 67 (2026-10-27): 26h and 26i are done (TUNE no longer timed as a
key-up; RX stays muted through TX unless MON is on).

Build 66 (2026-10-27): item 26 (a-g) is done - TX / RX switching. The
Flex reports the interlock as {type:'onAir'}; the page's send() starts the
RX gate on any setPTT true (MOX, FT8, JS8, SSTV, packet) and TUNE, and
returns to RX on setPTT false / TUNE off.

Build 65 (2026-10-27): items 3 (FT-991A), 19c, 19d, 19f, 24 and 25 are
done. 19c needed nothing new: the off-screen VFO tab and click-to-tune to
the nearest 1 kHz already worked on every radio with a scope. 19f: the page
already shows controls by the rig file's commands; Flex-only controls
appear only with the Flex's flexDsp report, and a radio without a spectrum
says so. Still open from 19: a, b, e (with item 4).
FT-991A notes for the user's test: CAT rate (menu 031) is found by the
probe; the EX menu numbers differ from the FT-710's so FlexWeb sends none
(set the USB / DATA audio menus on the radio); split (FT2 / FT3) is not
mapped yet.

Build 64 (2026-10-27): items 7, 22 and 23 are done. Item 7 is measured in
the page (arrival times of audio, scope frames and meter readings on the
same WebSocket) rather than stamped by the server; the SDR / Kiwi sources
will feed the same estimator. 22's terminal status page names are not done.

Build 63 (2026-10-27, "quick wins"): items 1, 2, 9, 10, 11, 12, 13, 14,
16, 17 and 18 are done. Still open: 3, 4, 5, 6, 7, 8, 15, 19, 20, 21.
Not done from those items: 17's per-feature CPU shares (NNR, Opus) and the
terminal status page; 18c (the optional FFT S-meter) - only if 18b is not
enough on the radio; 18b's dBFS-only fallback (the FLEX-6400 reports dBm).

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
16. RF-KIT to AetherSDR rfkit-tune-pr 934dcd6 (2026-10-24, the user's
    latest: one commit, same source as rfkit-tune 1199ed3; supersedes the
    v2 hand-off 53e95571; build 62 has 94b0496). Changes from v2, which
    win over a-h below: ATU MAN BYP / ATU BYP / ATU OFF neutral, not red
    (only NO TUNE is red); the ATU indicator leads the ATU line; RESET
    only while an error shows and asks for confirmation; OPERATE / TUNE /
    RESET equal width.
    Re-port RfkitProtocol / RfkitConnection / protocol test, then:
    a. Badge wording KPA1500-style "ATU" (replaces TUNER ON / MANUAL ...):
       ATU IN (AUTO, AUTO_TUNING*; green), ATU MAN (MANUAL + CL / LC;
       green), ATU MAN BYP (MANUAL + BYPASS; neutral), ATU BYP (BYPASS;
       neutral), ATU OFF (OFF; neutral), ATU NO TUNE (AUTO or MANUAL with
       setup NOT TUNED; red), "ATU -" neutral. BYPASS / OFF win over NOT TUNED; a tune in
       progress stays ATU IN. Info line "ATU <mode> <setup> L .. C ..".
    b. TUNE allowed in AUTO and MANUAL incl. MANUAL+BYPASS and NOT TUNED;
       not in BYPASS / OFF.
    c. Drive hint: outline TUNE in blue while /power forward is 4-39 W (the
       manual's tune drive range, 7.2); tooltip says 10 W or more is
       recommended below 10 W. Hint only, TUNE stays clickable.
    d. Fault banner "Error: <text>" = /data status plus the interface error
       when different; the plain STATUS tile goes. RESET enabled (red) only
       while a fault shows, disabled otherwise, with a confirmation.
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
21. Radio picker with hand-off for clubs (user, 2026-10-27; replaces the
    first draft; medium-hard). A RADIOS page on the club ("hub") FlexWeb
    lists every radio the club can use; picking one hands the user to the
    FlexWeb that should run it.
    a. Who runs a radio:
       - The radio already has a FlexWeb running at its site (registered
         with the hub, see b): hand off - open that server's page (same
         sign-in, see d). The hub never connects to that radio itself.
       - No FlexWeb at the radio and the hub can reach it (Flex over
         SmartLink or LAN, Icom LAN rigs over the network): the hub connects
         and runs it. The hub is then busy: every other radio that would
         need the hub shows "Hub busy - in use on <radio> by <user>" until
         the hub releases it (last user leaves + idle timeout, or the
         operator presses RELEASE; never while transmitting).
       - USB / serial radios (FT-991A, Icom USB, Kenwood ...) can only be
         run by a FlexWeb on the computer they are plugged into; listed when
         that server is registered, else not at all.
    b. Finding radios:
       - FlexWeb servers register with the hub (hub URL + club key in their
         settings): heartbeat every 30 s with server URL, radio model,
         serial / name, frequency, mode, signed-in users, TX holder,
         capabilities. Missing 3 heartbeats = offline.
       - Flex radios the hub can reach: SmartLink account list
         (--smartlink-radios path) and LAN discovery (UDP 4992). Matched to
         registered servers by radio serial; SmartLink's client list
         (gui_client_programs) also shows "FlexWeb" when one is connected.
       - Icom LAN rigs: entered by the admin (address, port, user).
    c. The page: one card per radio, Flex and non-Flex alike - model, name /
       callsign of the station, location, online / offline, frequency and
       mode, users and who has TX, "runs on <its server>" or "hub can run
       it". Capability badges from items 3, 4-6 and 19: CAT, SCOPE (built-in
       panadapter) or SDR SCOPE (RTL / Airspy / SDRplay / Kiwi feeding the
       panadapter), AMP, TX allowed for you. Buttons: OPEN (hand off), RUN
       HERE (hub hosts it; greyed when the hub is busy), VIEW (guest, item 2).
    d. Accounts across servers: sign in once on the hub; the hand-off passes
       a short-lived token signed with the club key, which the radio's
       server exchanges for its own session. Per-radio permission (view /
       RX / TX) set on the hub; each radio's own server can still refuse.
       Usage log on the hub: who used which radio, when, TX time (for the
       station licensee).
    e. Later, optional: the hub starts a separate FlexWeb process per radio
       it runs (own port), so it is not limited to one; booking of time
       slots.
    Depends on 19 (CAT / panadapter for all radios) for the non-Flex cards
    to be useful, and on 2 (guest view-only).
22. Who is connected (user, 2026-10-27; easy). The server pushes
    {type:'clients'} whenever a browser connects, signs in or leaves: one
    entry per browser with its user name (or "guest", or the address when
    login is off), whether it has audio on, holds TX or is talking. The
    page shows a small chip in the top bar ("3" with a people icon; hidden
    when this browser is alone), and a tap lists them: "tom (you)", "amy -
    TX", "2 guests". Guests see the count and the named users but not other
    guests' addresses. Also on the terminal status page (names instead of
    just the browser count).
23. Guests hear the transmitting operator (user, 2026-10-27; easy). Today a
    guest hears nothing while someone transmits: TX LISTEN is set by the
    page's setTxListen, which guests may not send, and the server's default
    for a browser that never sent it is OFF. Fix: a guest starts on
    OPERATOR (the operator's microphone straight through) at its own LISTEN
    VOL, like a signed-in user's default; guests may send setTxListen with
    OPERATOR or OFF (added to guestAllowed / GUEST_CMDS) but not ON-AIR,
    which switches the radio's monitor on. LISTEN VOL and DIGI LISTEN stay
    local to the guest's browser.
24. Fine step for Left / Right (user, 2026-10-27; easy). Today Left / Right
    tune a fixed 50 Hz (HZ_STEP), while Up / Down follow STEP and
    Shift + Up / Down the fast step. Give Left / Right their own FINE STEP
    setting (10 / 50 / 100 / 500 Hz, default 50), shown with the STEP pill
    (e.g. "STEP 1k / 50") and under FUNC > STEP, kept per browser like STEP.
    The on-screen fine-tune arrows use it too.
25. No sync delay while the user is acting (user, 2026-10-27; easy-medium).
    Scope sync holds the spectrum, waterfall and S meter back by the audio
    delay (200-400 ms), so dragging the scope, tuning (knob, wheel, keys,
    click-to-tune), changing band / mode / filter / span or zoom answered
    that much later than with audio off. Fix: any such input from this
    browser flushes the scope-sync and meter queues and shows them live;
    sync stays off until 1.5 s after the last input, then eases back in
    (the existing slew, ~50 ms per second, so the waterfall never jumps).
    While live, the scope is ahead of the audio by the audio delay - the
    trade the user chose. Inputs that do not change what the radio
    receives (volume, mute, panels) leave sync alone. The audio delay itself
    stays (it is the jitter buffer that stops dropouts).
    Switchable (user, 2026-10-27): FUNC > AUDIO > LIVE TUNING ON (default) /
    OFF, per browser (localStorage), next to SYNC SCOPE and SYNC TRIM; OFF =
    today's behaviour (always in sync). SYNC SCOPE OFF still turns sync off
    altogether, as in wfweb / AetherSDR.
26. TX / RX switching delays and the MOX blip (user, 2026-10-27; medium).
    Seen on the FLEX-6400 + RF2K-S, 3.578 USB JS8 (video
    FlexWeb_VE3NSV_20261027_181956_3578kHz_USB.mp4) and the server log
    (radio on the air 47-127 ms after xmit 1). Seven parts:
    a. TUNE: toggleTuneCarrier does not call flushAudio, so 200-400 ms of
       buffered RX audio keeps playing after TUNE starts. Flush there too.
    b. MOX release: releasePTT waits a fixed 300 ms before setPTT false.
       Wait only for the mic audio actually still in flight (what was sent
       minus what the radio has played), then unkey.
    c. RX after TX: the jitter buffer refills to its full target before RX
       audio is heard again. On TX -> RX flush and fast-start with a short
       fade-in.
    d. MOX blip: on PTT press the page flushes, but the radio keeps
       streaming RX audio until it is on the air (47-127 ms), and that
       slice plays alone as a blip. Server tells the page when the
       interlock reports TRANSMITTING (and when it drops); the page mutes
       RX audio from the PTT press until then and through the TX.
    e. Held mic audio: flexCommander holds DAX TX audio until on the air,
       then sends it at once. One press held 336 ms for a 100 ms key-up
       (mic started before xmit 1), leaving the voice ~1/3 s behind for the
       whole over and risking a cut tail at unkey. Keep only the last
       key-up's worth (drop older held packets).
    f. JS8 trailing silence: js8.mjs synthesize() returns a full slot, so
       the last frame of every TX carries ~2 s of silence (Normal 15 s slot,
       signal ends 13.1 s; Fast / JS8 40 ~2 s; Slow ~4.4 s), plus the 200 ms
       wait in streamDigiAudio's finish(), before setPTT false and the
       panel's return to RX. Trim the silence after the last frame's final
       symbol (keep the gaps between frames, they hold slot alignment).
       JS8Call unkeys right after the last tone.
    g. Jitter buffer growth: the deliberate flush on PTT / TUNE / digital TX
       is counted as an underrun ("[Audio] underrun #15: 61 ms gap, buffer
       132 -> 135 ms"), so the target creeps up a little every over. Do not
       count a flush (or the TX gap after it) as an underrun.
    Follow-up after build 66 (user's log and screen recording, 15:21-15:23):
    h. Key-up time polluted by TUNE: TUNE (and anything else that keys the
       radio without our xmit 1) logged "on the air 3190 / 5510 / 6635 /
       9211 ms after xmit 1" - timed from the last MOX. Those readings go
       into keyDelayMs (capped 2000), which drives txTailMs (MOX release up
       to 500 ms) and the FT8 / JS8 TX lead (up to 1.5 s early). Time only
       a key-up we asked for (txRequested), once, then invalidate the timer.
    i. Blip still there, smaller and later: RX audio cuts at the press,
       then ~150-300 ms later a 50-100 ms burst of RX, then silence. The
       Flex's RX audio stream runs behind its interlock, so a few RX packets
       still arrive after TRANSMITTING. Keep RX silent through the whole TX
       when MON is off (the stream carries only zeros then); with MON on,
       open 250 ms after on-air. End the gate on unkey, on onAir false and
       on the rig's own return to RX (TX timeout).
27. Panel buttons toggle (user, 2026-10-27; easy). The AUDIO button in the
    TX applet (desktop layout) opens FUNC > AUDIO / TX, but clicking it
    again does nothing - only the panel's exit button closes it. Make it a
    toggle: open when closed, close (back to the default view) when that
    panel is already open; the exit button keeps working. Every tab /
    button that opens a panel behaves alike (user: "other tabs also").
    Already toggle: MODE..., FUNC, the gain tile, MEMORIES. To change:
    AUDIO, PROC..., NNR..., EQ, RX DSP, ANT, BAND, AMP, SPAN and the
    frequency entry - one helper, togglePanel(name), used by all of them.
    Show the button lit while its panel is open.
28. CLICK TO START needs several clicks (user, 2026-10-27; easy). The
    splash (#startupOverlay, index.html ~2428) is drawn before ten
    synchronous scripts (rig-transport.js ... cw-keyer.js, ~3171-3181) and
    the main inline script; the click listener is attached at the very end
    (~16189). Over the WAN each script is a round trip, so clicks in the
    first seconds hit a button with no listener and are lost. Fix: the
    button reads "LOADING..." (dimmed) until the main script runs, then
    "CLICK TO START"; an inline onclick records an early click and the
    start handler runs it as soon as it is attached (the click gives the
    page user activation, so the AudioContext may still start). One click
    always works.
29. Launcher shows the rig connected after --on-demand released it (user,
    2026-10-27; easy). LogParser only knows "Loading Rig: ..." (RigModel);
    nothing classifies "On-demand: no browser for N s, releasing the rig"
    or "On-demand: browser connected, connecting to the rig", so the
    launcher keeps the radio shown as connected until restart. Add both
    lines to LogParser::classify (new events, e.g. RigReleased /
    RigReconnecting) and show "Released - waiting for a browser" / the
    radio again in the main window; tests in tests/launcher_test.cpp. Note
    for the user: any open FlexWeb tab holds the rig, even one on the
    CLICK TO START splash (the WebSocket connects before the click).
30. DISCONNECT button in the browser (user, 2026-10-27; easy-medium).
    Today the only way off is closing the tab, and the only "Disconnect
    LAN" button (#lanConnBtn, disconnectLan) sits on the "Radio is powered
    off" screen. Add DISCONNECT to the top bar (next to the people / CPU
    chips) and FUNC:
    - This browser: stop RX audio and the mic, unkey if it holds TX, close
      the WebSocket and show a "Disconnected - RECONNECT" screen (no
      automatic reconnect while it is shown). The tab then no longer holds
      the radio for --on-demand.
    - Last browser on the server: also release the radio at once
      (disconnectLan) instead of after the --on-demand idle time, so the
      Flex is free for SmartSDR / another client right away. Ask first
      when other browsers are connected ("2 others are on - disconnect only
      this browser?"); never release the radio from under them.
    - Guests get the same button (it only affects their own session).
    - RECONNECT reopens the WebSocket; with --on-demand that reconnects the
      radio as today.
31. Movable divider between spectrum and waterfall (user, 2026-10-27;
    easy-medium). Today the split is fixed: #spectrumCanvas 35 % / waterfall
    65 % in CSS (index.html ~478-495, #spanEdgeLeft/Right at top: 35 %)
    and resizeCanvases() (~10887, spectH = availH * 0.35, also used with
    the CW bar). Add a drag bar on the line between them (a few px tall,
    row-resize cursor, a grip mark; touch too): drag sets the spectrum
    share, clamped 15-80 %, kept per browser (localStorage wfSpecSplit);
    double-click resets to 35 %. One variable drives the CSS (a --spec-split
    custom property), resizeCanvases and the span-edge labels, and the CW
    bar case keeps working. As AetherSDR's panadapter / waterfall splitter.
    Also under FUNC > SPAN as a tile (SPLIT 25 / 35 / 50 / 65 %) for
    keyboard-only use.
32. RF2K-S forward power peak marker (user, 2026-10-27; easy). The FWD
    bar in the AMP applet (aeRenderAmp, index.html ~7181) and in FUNC > AMP
    (ampBar, ~7867) shows only the latest reading, so on SSB it jumps
    around and the peaks are hard to read. Add a peak marker like the
    S-meter's: a thin bright tick at the highest FWD seen, held ~1.5 s,
    then falling smoothly (about 400 W/s) toward the current reading;
    cleared at unkey (FWD back under 1 W). Show the peak in the value too
    ("620 W  pk 980"). Page-only: the peak is kept from each {type:'amp'}
    update (the server polls the amp every kPollIntervalMs), and the
    marker is drawn on its own element so it can fall between updates
    without rebuilding the applet. Same marker on REF.
33. Sidebar disappears when the browser is shrunk (user, 2026-10-27;
    medium). The desktop layout (slice strip + 320 px applet sidebar) only
    applies while AE_DESKTOP_MQ matches: (min-width: 1100px) and
    (min-height: 560px) and landscape (index.html ~6744); below that (a
    narrowed window, a portrait tablet, a short window) the page falls back
    to the classic layout and the RX / TX / EQ / AMP applets are gone.
    Fix, keeping the phone layout (portrait, <= 600 px) as it is:
    - Desktop layout from 700 px wide and 420 px tall, either orientation.
    - Sidebar width follows the window: 320 px from 1280 px, 260 px from
      1000 px (applet rows tighten: smaller fonts / gaps in aether.css).
    - Under 1000 px the sidebar becomes a drawer over the right of the
      scope: a PANELS (&#9776;) button at the right end of the slice strip
      opens / closes it, Escape or a click on the scope closes it, and the
      last state is kept per browser. Same applets, same ids (moved, not
      copied, as today).
    - aeLayout() re-runs on resize (matchMedia listeners), so dragging the
      window edge switches cleanly; resizeCanvases() follows.
