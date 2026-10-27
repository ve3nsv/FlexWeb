# Patches to submodules

Applied by every CI build (`.github/workflows/build.yml`, "Build RADE") and by
`build.bat` before the submodule is compiled. For a local Linux / macOS build,
apply once before building RADE:

    git -C resources/radae_nopy apply ../../patches/rade_c-eoo-demod.patch

## rade_c-eoo-demod.patch (resources/radae_nopy = freedv/rade_c)

`rade_ofdm_demod_eoo()` (V1) mismatches the end-of-over frame the transmitter
builds (`rade_ofdm_init` / `rade_tx_state_eoo`: P E D D D E, Ns = 4):

- it takes symbol Ns (data) as the second E pilot instead of Ns+1, so the
  phase estimate mixes in a data symbol;
- it extracts symbols 2..Ns-1 only, writing 120 of the 180 values
  `rade_rx_n_eoo_bits()` says it returns. `rade_rx_process` copies all 180
  from a stack array, so the last 60 are whatever the stack held.

On Linux that stack happened to be benign; on Windows and macOS it held huge
values, which swamped the soft-decision scaling of the callsign decoder: no
RADE callsign ever decoded (FlexWeb punch list 34c; macOS CI caught it with
tests/test_rade_eoo.py). Not yet fixed upstream (rade_c 22d6e84).
