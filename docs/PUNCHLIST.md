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
