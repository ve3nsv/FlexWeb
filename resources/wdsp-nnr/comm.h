/*  comm.h — FlexWeb shim for the WDSP NNR subset.

WDSP's own comm.h pulls in the whole library (channels, Windows threading,
FFTW). FlexWeb builds only the neural noise reduction block (nnr.c, nnet.c,
nnio.c and the two built-in models), so this header supplies just what those
files use. The RXA wrappers at the end of nnr.c compile against the dummy rxa[]
and ch[] below and are never called: FlexWeb drives an NNR object directly
(src/nnrfilter.cpp), as AetherSDR's NnrFilter does.
*/
#ifndef _wfweb_wdsp_comm_h
#define _wfweb_wdsp_comm_h

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fftw3.h"

#define PORT
#ifndef _MSC_VER
#define __declspec(x)
#endif

/* WDSP's debug print (a Windows OutputDebugString wrapper) - silent here.
   POSIX stdio declares dprintf(int, ...): replace it for these files. */
#ifdef dprintf
#undef dprintf
#endif
#define dprintf(...) ((void)0)

void* wfweb_nnr_malloc0 (size_t size);
void  wfweb_nnr_free (void* p);
#define malloc0(size) wfweb_nnr_malloc0 (size)
#define _aligned_free(p) wfweb_nnr_free (p)

typedef double complex[2];                 /* WDSP's interleaved I/Q sample */

typedef struct { int unused; } CRITICAL_SECTION;
#define EnterCriticalSection(cs) ((void)(cs))
#define LeaveCriticalSection(cs) ((void)(cs))

#include "nnr.h"

#ifndef __cplusplus
/* Private names, so these dummies cannot meet another rxa / ch at link time. */
#define rxa wfweb_nnr_rxa
#define ch  wfweb_nnr_ch
typedef struct { struct { NNR p; } nnr; } wfweb_rxa_t;
typedef struct { CRITICAL_SECTION csDSP; } wfweb_ch_t;
extern wfweb_rxa_t rxa[1];
extern wfweb_ch_t ch[1];
#endif

#endif
