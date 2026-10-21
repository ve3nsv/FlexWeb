# FlexWeb next batch (aether-look branch)

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
   - Classic FreeDV (700D / 700E / 1600): same treatment unless the user says
     otherwise (mention it).
3. FM and NFM (and DFM on a Flex): the same noise filters off while the slice
   is in FM, put back when it leaves FM. NNR currently counts FM as a voice
   mode (flexCommander::nnrModeOk) - take FM, NFM and DFM out of it. Only the
   filter settings change here, not the mode or the receive filter width.
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
