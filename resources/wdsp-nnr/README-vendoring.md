# WDSP NNR subset (FlexWeb)

Neural Noise Reduction from WDSP 2.10 (Warren Pratt, NR0V; GPLv2 or later,
see LICENSE), TAPR/OpenHPSDR-wdsp commit
`b02d5bac675dd2f33ec2bab2b339f79a597c47dd`, as vendored by AetherSDR
(`third_party/wdsp/upstream`). Only the NNR block is built:

- `nnr.c`, `nnr.h`, `nnet.c`, `nnet.h`, `nnet_profile.h`, `nnio.c`, `nnio.h`,
  `nnr_model_0.c` (Standard), `nnr_model_1.c` (Premium) — unmodified.
- `comm.h` — FlexWeb shim replacing WDSP's comm.h: allocation, the `complex`
  sample type, a silent `dprintf`, and dummy `rxa[]` / `ch[]` so the RXA
  wrappers in nnr.c compile (they are never called).
- `fftw3.h`, `fftw_shim.c` — the four FFTW calls nnr.c makes, as a radix-2
  real FFT (NNR's transform is 512 points), so no FFTW dependency.

`src/nnrfilter.cpp` drives one NNR object directly, as AetherSDR's
`NnrFilter` does. To update: copy the files above from a newer WDSP and check
that nnr.c still uses only what comm.h / fftw3.h provide.
