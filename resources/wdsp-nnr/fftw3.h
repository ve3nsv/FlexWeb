/*  fftw3.h — FlexWeb's stand-in for the four FFTW calls nnr.c makes.

NNR transforms 512 real samples (its STFT at the 16 kHz network rate), so a
plain radix-2 FFT (fftw_shim.c) does the job without FFTW on every platform.
Same conventions as FFTW: r2c gives n/2+1 bins, c2r is unnormalised.
Only power-of-two sizes are supported; a plan for any other size is NULL.
*/
#ifndef _wfweb_fftw3_shim_h
#define _wfweb_fftw3_shim_h

typedef double fftw_complex[2];
typedef struct wfweb_fftw_plan_s* fftw_plan;

#define FFTW_MEASURE 0u
#define FFTW_PATIENT (1u << 5)
#define FFTW_ESTIMATE (1u << 6)

fftw_plan fftw_plan_dft_r2c_1d (int n, double* in, fftw_complex* out, unsigned flags);
fftw_plan fftw_plan_dft_c2r_1d (int n, fftw_complex* in, double* out, unsigned flags);
void fftw_execute (const fftw_plan p);
void fftw_destroy_plan (fftw_plan p);

#endif
