/* FlexWeb: FFTW stand-in for the WDSP NNR subset (see fftw3.h), and the
   allocator / dummy channel state comm.h declares. */
#include "comm.h"

wfweb_rxa_t rxa[1];
wfweb_ch_t ch[1];

void* wfweb_nnr_malloc0 (size_t size)
{
    return calloc (1, size ? size : 1);
}

void wfweb_nnr_free (void* p)
{
    free (p);
}

struct wfweb_fftw_plan_s {
    int n;
    int inverse;           /* 0 = r2c, 1 = c2r */
    double* rin;
    fftw_complex* cin;
    double* rout;
    fftw_complex* cout;
    double* re;            /* n-point complex work buffer */
    double* im;
    double* cosT;          /* twiddles, n/2 */
    double* sinT;
    int* rev;              /* bit reversal */
};

static int is_pow2 (int n) { return n >= 2 && (n & (n - 1)) == 0; }

static fftw_plan make_plan (int n, int inverse)
{
    fftw_plan p;
    int i, bits = 0;
    if (!is_pow2 (n)) return NULL;
    p = (fftw_plan) calloc (1, sizeof (*p));
    p->n = n;
    p->inverse = inverse;
    p->re = (double*) calloc ((size_t)n, sizeof (double));
    p->im = (double*) calloc ((size_t)n, sizeof (double));
    p->cosT = (double*) calloc ((size_t)n / 2, sizeof (double));
    p->sinT = (double*) calloc ((size_t)n / 2, sizeof (double));
    p->rev = (int*) calloc ((size_t)n, sizeof (int));
    while ((1 << bits) < n) bits++;
    for (i = 0; i < n; i++)
    {
        int r = 0, b;
        for (b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        p->rev[i] = r;
    }
    for (i = 0; i < n / 2; i++)
    {
        p->cosT[i] = cos (2.0 * 3.14159265358979323846 * i / n);
        p->sinT[i] = sin (2.0 * 3.14159265358979323846 * i / n);
    }
    return p;
}

fftw_plan fftw_plan_dft_r2c_1d (int n, double* in, fftw_complex* out, unsigned flags)
{
    fftw_plan p = make_plan (n, 0);
    (void)flags;
    if (p) { p->rin = in; p->cout = out; }
    return p;
}

fftw_plan fftw_plan_dft_c2r_1d (int n, fftw_complex* in, double* out, unsigned flags)
{
    fftw_plan p = make_plan (n, 1);
    (void)flags;
    if (p) { p->cin = in; p->rout = out; }
    return p;
}

/* In-place iterative radix-2 on re/im; sign -1 forward, +1 inverse. */
static void fft_core (fftw_plan p, int sign)
{
    int n = p->n, i, len;
    double* re = p->re;
    double* im = p->im;
    for (i = 0; i < n; i++)
    {
        int j = p->rev[i];
        if (j > i)
        {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1)
    {
        int half = len >> 1, step = n / len, k, j;
        for (k = 0; k < n; k += len)
        {
            for (j = 0; j < half; j++)
            {
                double wr = p->cosT[j * step];
                double wi = sign * p->sinT[j * step];
                int a = k + j, b = a + half;
                double xr = re[b] * wr - im[b] * wi;
                double xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
}

void fftw_execute (const fftw_plan p)
{
    int n, i;
    if (!p) return;
    n = p->n;
    if (!p->inverse)
    {
        for (i = 0; i < n; i++) { p->re[i] = p->rin[i]; p->im[i] = 0.0; }
        fft_core (p, -1);
        for (i = 0; i <= n / 2; i++) { p->cout[i][0] = p->re[i]; p->cout[i][1] = p->im[i]; }
    }
    else
    {
        /* Rebuild the Hermitian spectrum from the n/2+1 bins. */
        for (i = 0; i <= n / 2; i++) { p->re[i] = p->cin[i][0]; p->im[i] = p->cin[i][1]; }
        for (i = n / 2 + 1; i < n; i++) { p->re[i] = p->cin[n - i][0]; p->im[i] = -p->cin[n - i][1]; }
        fft_core (p, +1);
        for (i = 0; i < n; i++) p->rout[i] = p->re[i];
    }
}

void fftw_destroy_plan (fftw_plan p)
{
    if (!p) return;
    free (p->re); free (p->im); free (p->cosT); free (p->sinT); free (p->rev);
    free (p);
}
