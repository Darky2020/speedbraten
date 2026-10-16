// Float math with the same result bits on every platform, for the simulation.
// Only + - * / and sqrt are used, so with -ffp-contract=off and no fast-math every
// compiler/CPU/OS produces identical results. libm sinf/atan2f/powf differ between
// glibc, macOS and Windows, and even between CPUs at runtime.
//
// The functions must not be named after libm functions: compilers constant-fold
// known libm names using the host's libm.

#ifndef DETMATH_H
#define DETMATH_H

#include <math.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

static inline float detsin_kernel(float x) { float z = x*x; return x + x*z*(-1.6666667163e-01f + z*(8.3333337680e-03f + z*(-1.9841270114e-04f + z*2.7557314297e-06f))); }
static inline float detcos_kernel(float x) { float z = x*x; return 1.0f + z*(-0.5f + z*(4.1666667908e-02f + z*(-1.3888889225e-03f + z*2.4801587642e-05f))); }

// x in radians
static inline void detsincos(float x, float &s, float &c)
{
    if(!(x == x)) { s = c = x; return; }
    const float TWOPI = 6.2831853072f;
    // keeps the quadrant count within int range; fmodf is exact
    if(!(fabsf(x) < 1e5f)) { x = fmodf(x, TWOPI); if(!(x == x)) { s = c = x; return; } }
    const float PIO2_HI = 1.5707962513e+00f, PIO2_LO = 7.5497894159e-08f, TWOOPI = 6.3661977237e-01f;
    float fk = x * TWOOPI;
    int k = int(fk >= 0 ? fk + 0.5f : fk - 0.5f);
    float r = (x - float(k)*PIO2_HI) - float(k)*PIO2_LO;
    float sr = detsin_kernel(r), cr = detcos_kernel(r);
    switch(k & 3)
    {
        case 0: s = sr;  c = cr;  break;
        case 1: s = cr;  c = -sr; break;
        case 2: s = -sr; c = -cr; break;
        default: s = -cr; c = sr; break;
    }
}

static inline float detsin(float x) { float s, c; detsincos(x, s, c); return s; }
static inline float detcos(float x) { float s, c; detsincos(x, s, c); return c; }
static inline float dettan(float x) { float s, c; detsincos(x, s, c); return s/c; }

static inline float detatan(float x)
{
    bool inv = fabsf(x) > 1.0f;
    float t = inv ? 1.0f/x : x, z = t*t;
    float p = t*(0.99997726f + z*(-0.33262347f + z*(0.19354346f + z*(-0.11643287f + z*(0.05265332f + z*-0.01172120f)))));
    if(inv) p = (x > 0 ? 1.5707963268f : -1.5707963268f) - p;
    return p;
}

static inline float detatan2(float y, float x)
{
    if(!(x == x) || !(y == y)) return x + y;
    if(x > 0) return detatan(y/x);
    if(x < 0) return detatan(y/x) + (y >= 0 ? 3.1415926536f : -3.1415926536f);
    return y > 0 ? 1.5707963268f : (y < 0 ? -1.5707963268f : 0.0f);
}

// argument clamped to [-1, 1] instead of returning NaN
static inline float detasin(float x) { x = x > 1 ? 1.0f : (x < -1 ? -1.0f : x); return detatan2(x, sqrtf(1 - x*x)); }
static inline float detacos(float x) { x = x > 1 ? 1.0f : (x < -1 ? -1.0f : x); return detatan2(sqrtf(1 - x*x), x); }

// x^(t/20) as used by friction; exact for the 5 ms simulation tick
static inline float detfrictionpow(float x, int t)
{
    if(t == 5) return sqrtf(sqrtf(x));
    if(t == 20) return x;
    return powf(x, t/20.0f);
}

// round to nearest, no flush-to-zero, no default-NaN, all exceptions masked
static inline void detfpmode()
{
#if defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr(0x1F80);
#elif defined(__aarch64__)
    unsigned long fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr &= ~((3UL<<22) | (1UL<<24) | (1UL<<25) | (1UL<<19));
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
}

#endif
