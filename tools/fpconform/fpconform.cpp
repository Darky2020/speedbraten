// Floating-point conformance probe.
//
// Runs fixed, seeded workloads that mirror the game's simulation maths and prints one
// FNV-1a hash of the raw result bits per test. Build it for every target/flag combination
// and diff the outputs: a differing line names the operation class that is not
// bit-reproducible on that platform.

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "../../src/shared/detmath.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#include <pmmintrin.h>
#define HAVE_MXCSR 1
#endif

typedef uint32_t u32;
typedef uint64_t u64;

static const float PI_F = 3.14159265358979323846f;

static u32 bits(float f) { u32 u; memcpy(&u, &f, 4); return u; }
static float frombits(u32 u) { float f; memcpy(&f, &u, 4); return f; }

struct hasher
{
    u64 h = 1469598103934665603ULL;
    u64 n = 0;
    void add(u32 v) { for(int i = 0; i < 4; i++) { h ^= (v >> (i*8)) & 0xFF; h *= 1099511628211ULL; } n++; }
    void addf(float f) { add(bits(f)); }
    // NaN payload/sign is not specified by IEEE 754 and differs between x86 and ARM.
    void addf_canon(float f) { add(f != f ? 0x7FC00000u : bits(f)); }
};

struct rng
{
    u64 s;
    explicit rng(u64 seed) : s(seed ? seed : 1) {}
    u32 next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return u32(s >> 32); }
    float unit() { return (next() >> 8) * (1.0f / 16777216.0f); }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
    float anybits() { return frombits(next()); }
    float denormal() { return frombits((next() & 0x007FFFFFu) | (next() & 0x80000000u)); }
};

static void report(const char *name, const hasher &h)
{
    printf("%-28s %016llx  n=%llu\n", name, (unsigned long long)h.h, (unsigned long long)h.n);
}

static const int N = 200000;

// ---- basic IEEE ops -------------------------------------------------------

static void test_basic_normal()
{
    rng r(1); hasher h;
    for(int i = 0; i < N; i++)
    {
        float a = r.range(-1e4f, 1e4f), b = r.range(-1e4f, 1e4f);
        if(b == 0) b = 1;
        h.addf(a + b); h.addf(a - b); h.addf(a * b); h.addf(a / b); h.addf(sqrtf(fabsf(a)));
    }
    report("basic_ops_normal", h);
}

static void test_basic_anybits()
{
    rng r(2); hasher h;
    for(int i = 0; i < N; i++)
    {
        float a = r.anybits(), b = r.anybits();
        h.addf_canon(a + b); h.addf_canon(a - b); h.addf_canon(a * b); h.addf_canon(a / b); h.addf_canon(sqrtf(a));
    }
    report("basic_ops_anybits_canonnan", h);
}

static void test_nan_payload()
{
    rng r(3); hasher h;
    for(int i = 0; i < N; i++)
    {
        float a = r.anybits(), b = r.anybits();
        h.addf(a + b); h.addf(a * b); h.addf(a / b); h.addf(sqrtf(a));
    }
    report("nan_payload_raw", h);
}

static void test_denormals()
{
    rng r(4); hasher h;
    for(int i = 0; i < N; i++)
    {
        float a = r.denormal(), b = r.denormal(), c = r.range(-2.0f, 2.0f);
        h.addf(a + b); h.addf(a * c); h.addf(a * 0.5f); h.addf(c * 1e-38f); h.addf(a / 3.0f);
    }
    report("denormals", h);
}

// ---- libm transcendentals ---------------------------------------------------

static void test_libm()
{
    {
        rng r(10); hasher h;
        for(int i = 0; i < N; i++) { float x = r.range(-720.0f, 720.0f) * (PI_F/180.0f); h.addf(sinf(x)); h.addf(cosf(x)); }
        report("libm_sinf_cosf", h);
    }
    {
        rng r(11); hasher h;
        for(int i = 0; i < N; i++) { float y = r.range(-1000, 1000), x = r.range(-1000, 1000); h.addf(atan2f(y, x)); }
        report("libm_atan2f", h);
    }
    {
        rng r(12); hasher h;
        for(int i = 0; i < N; i++) { float x = r.range(-1.0f, 1.0f); h.addf(asinf(x)); }
        report("libm_asinf", h);
    }
    {
        rng r(13); hasher h;
        for(int i = 0; i < N; i++) { float b = r.range(0.5f, 1.0f), e = r.range(0.0f, 0.25f); h.addf(powf(b, e)); }
        report("libm_powf", h);
    }
    {
        rng r(14); hasher h;
        for(int i = 0; i < N; i++) { float b = r.range(0.5f, 1.0f), e = r.range(0.0f, 0.25f); h.addf(float(pow(double(b), double(e)))); }
        report("libm_pow_double", h);
    }
}

// ---- the game's own functions (src/shared/detmath.h) -------------------------

static void test_det()
{
    {
        rng r(10); hasher h;
        for(int i = 0; i < N; i++) { float x = r.range(-720.0f, 720.0f) * (PI_F/180.0f), s, c; detsincos(x, s, c); h.addf(s); h.addf(c); }
        report("det_sincos", h);
    }
    {
        rng r(14); hasher h;
        for(int i = 0; i < N; i++) { float x = r.range(-1e7f, 1e7f), s, c; detsincos(x, s, c); h.addf(s); h.addf(c); }
        report("det_sincos_large", h);
    }
    {
        rng r(11); hasher h;
        for(int i = 0; i < N; i++) { float y = r.range(-1000, 1000), x = r.range(-1000, 1000); h.addf(detatan2(y, x)); }
        report("det_atan2", h);
    }
    {
        rng r(12); hasher h;
        for(int i = 0; i < N; i++) { float x = r.range(-1.1f, 1.1f); h.addf(detasin(x)); h.addf(detacos(x)); }
        report("det_asin_acos", h);
    }
    {
        rng r(13); hasher h;
        for(int i = 0; i < N; i++) { float b = r.range(0.5f, 1.0f); h.addf(detfrictionpow(b, 5)); }
        report("det_frictionpow", h);
    }
}

// ---- contraction-sensitive expression ---------------------------------------

static void test_contraction()
{
    rng r(20); hasher h;
    for(int i = 0; i < N; i++)
    {
        float a = r.range(-100, 100), b = r.range(-100, 100), c = r.range(-100, 100);
        h.addf(a*b + c);
    }
    report("mul_add_expression", h);
}

// ---- physics-shaped composite ------------------------------------------------

struct vec3 { float x, y, z; };

template<bool OWN>
static void vecfromyawpitch(float yaw, float pitch, int move, int strafe, vec3 &m)
{
    const float RAD = PI_F/180.0f;
    float sy, cy, sp, cp;
    if(OWN) { detsincos(RAD*yaw, sy, cy); detsincos(RAD*pitch, sp, cp); }
    else { sy = sinf(RAD*yaw); cy = cosf(RAD*yaw); sp = sinf(RAD*pitch); cp = cosf(RAD*pitch); }
    m.x = move*-sy*cp + strafe*cy;
    m.y = move* cy*cp + strafe*sy;
    m.z = move*sp;
}

template<bool OWN>
static void test_physics(const char *name)
{
    rng r(30); hasher h;
    vec3 vel = {0, 0, 0}, pos = {512, 512, 512};
    float yaw = 0;
    for(int tick = 0; tick < N; tick++)
    {
        yaw += r.range(-3.0f, 3.0f);
        int move = int(r.next() % 3) - 1, strafe = int(r.next() % 3) - 1;
        vec3 m;
        vecfromyawpitch<OWN>(yaw, 0, move, strafe, m);
        float len = sqrtf(m.x*m.x + m.y*m.y + m.z*m.z);
        if(len > 0) { m.x /= len; m.y /= len; m.z /= len; }
        float speed = 100.0f * 1.3f;
        vec3 d = {m.x*speed, m.y*speed, m.z*speed};
        float fric = (tick & 64) ? 6.0f : 30.0f;
        float t = OWN ? detfrictionpow(1 - 1/fric, 5) : float(pow(1 - 1/fric, 5/20.0f));
        vel.x = d.x + (vel.x - d.x)*t;
        vel.y = d.y + (vel.y - d.y)*t;
        vel.z = d.z + (vel.z - d.z)*t;
        pos.x += vel.x * 0.005f; pos.y += vel.y * 0.005f; pos.z += vel.z * 0.005f;
        h.addf(pos.x); h.addf(pos.y); h.addf(pos.z);
    }
    report(name, h);
}

static void run_all(const char *mode)
{
    printf("# mode=%s\n", mode);
    test_basic_normal();
    test_basic_anybits();
    test_nan_payload();
    test_denormals();
    test_libm();
    test_det();
    test_contraction();
    test_physics<false>("physics_libm");
    test_physics<true>("physics_det");
}

int main()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    const char *arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    const char *arch = "x86_64";
#else
    const char *arch = "unknown";
#endif
#if defined(__FAST_MATH__)
    const char *fm = "fast-math";
#else
    const char *fm = "strict";
#endif
    printf("# arch=%s math=%s\n", arch, fm);
#ifdef HAVE_MXCSR
    unsigned int saved = _mm_getcsr();
    printf("# mxcsr_at_start=%08x\n", saved);
    _mm_setcsr(0x1F80);
    run_all("mxcsr_default");
    _mm_setcsr(0x1F80 | 0x8040);
    run_all("mxcsr_ftz_daz");
    _mm_setcsr(saved);
#else
    run_all("default");
#endif
    return 0;
}
