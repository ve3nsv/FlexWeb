# FlexWeb next batch (aether-look branch)

The previous 15-item batch shipped in 1.0.55.

Digital modes get a clean receive path: JS8, FT8 / FT4 and FreeDV RADE.

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
