# FlexWeb next batch (aether-look branch)

Agreed with the user 2026-10-20. Build and test each item, one CI build at the end.
Suggested order: 5, 6, 7 first (daily use), NNR (3) last.

1. FreeDV Reporter message box — page field -> FreeDVReporter::setMessage (exists, unused), saved on the server.
2. AM carrier level — slider in the TX applet (`transmit set am_carrier=`).
3. NNR neural NR (AetherSDR: src/core/NnrFilter.*, third_party/wdsp nnr.c + nnr_model_0/1.c, needs FFTW3 on Windows CI).
   Server-side on RX audio; NNR button next to NB/NR/ANF; panel: Strength, Model Standard/Premium, reset,
   Advanced (Alpha, Knee, Tau, Max gain, Attack, Release), Method line, NR GAIN trace (post/pre RMS).
   Off by default, auto-off in CW/digital modes. Measure CPU (Pi 4 vs Pi 5 question).
4. Recorder sidebar — recorder.js video column (~280 px) with RX/TX/amp values + mic LVL; video only.
5. Filter WIDTH + SHIFT on Flex — page hides width/PBT because hasFilterSettings = funcPBTInner present;
   server already does funcFilterWidth; map PBT to a passband shift (filter_lo/filter_hi).
6. AGC mode Off/Slow/Med/Fast — server funcAGC exists, page has no control.
7. TUNE carrier + tune power (`transmit tune 1`, `transmit set tunepower=`), alongside ATU start.
8. CW: break-in + delay, iambic A/B, sidetone, CW monitor level (`cw break_in`, `cw break_in_delay`,
   `cw iambic`, `cw mode`, `cw sidetone`, `transmit set mon_gain_cw=`).
9. NB / NR / ANF levels (`nb_level`, `nr_level`, `anf_level`).
10. APF (`apf`, `apf_level`).
11. Radio band memory — band keys via `display pan set <pan> band=<n>` instead of the page's local list.
12. XIT (`xit_on`, `xit_freq`).
13. TNF and WNB (`tnf ...`, `display pan set wnb= wnb_level=`).
14. Max power and tune power limits (`transmit set max_power_level=`, `tunepower=`).
15. FlexWeb branding everywhere the user sees it (keep internal names so settings survive):
    - Page: splash/power-off version spans ("wfweb"), the local-memories badge and tooltips, the
      REMOTE-jack hint, console banner, logbook export name (wfweb_<call>_….adi -> FlexWeb_…).
      Keep: localStorage keys, the "WFWEB" waterfall palette id (rename label only), worklet class names.
    - Server text: --smartlink-login hint, "settings cleared", "wfweb stopped", login warning
      ("wfweb --add-user"), self-signed cert CN, the "power off from wfweb" log line.
    - Flex: SmartLink `application register name=FlexWeb`; send `client program FlexWeb` (and station
      name) so SmartSDR / the radio list show FlexWeb instead of a blank / wfweb.
    - Launcher: window/tray/message-box titles and texts, product/description/company in the .exe
      resources (QMAKE_TARGET_PRODUCT etc.).
    - Download names: zip/deb/AppImage names wfweb-* -> FlexWeb-* in build.yml (update the user's
      .bat files' WFWEB_DIR accordingly) — approved by the user; give the new link and .bat edits.
    - README / docs text where it describes this fork.
    - Keep unchanged: wfweb.exe / wfweb-launcher.exe file names (the .bat files and launcher call
      them), QSettings("wfweb", …) organisation/app names and %APPDATA%\wfweb (user's settings,
      users, SmartLink sign-in), WFWEB_* env vars and macros, upstream credit ("based on wfweb").

Skipped by the user: VOX / VOX delay, NAT hole punching.
