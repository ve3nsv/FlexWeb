# FlexWeb next batch (aether-look branch)

The previous 15-item batch shipped in 1.0.55.

1. JS8 always on USB. The panel only switches to USB when the mode is already
   known at open (`currentMode !== 'USB'` in js8-panel.mjs open()); after a page
   reload that restores the open panel, the mode is not known yet and the switch
   is skipped. Enforce USB once the mode arrives, and whenever it changes while
   the panel is open.
2. Receive DSP off while JS8 is open: NB, NR, ANF, NNR, APF, WNB (and, on a Flex,
   any TNF that sits in the passband), plus a filter wide enough for the JS8
   passband. Remember each setting at open and put it back at close, like the
   VFO snapshot (saveVfoSnapshot / restoreVfoSnapshot).
   - Ask the user: same for the FT8/FT4 panel?
