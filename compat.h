/*
 * compat.h - fixed-point helpers shared by the SH-2 build and the desktop
 * oracle.  Build-engine math (mulscale/divscale/...) is expressed with
 * 64-bit intermediates; on the SH-2 the common cases use dmuls.l/xtrct and
 * the on-chip 64/32 hardware divider (SH7604 DIVU).
 *
 * NOTE: the PWM mixer interrupt never touches the divider, so the renderer
 * may use it freely on both CPUs (each SH-2 has its own DIVU).
 */
#ifndef COMPAT_H
#define COMPAT_H

#include <stdint.h>
#include <string.h>

#ifdef __32X__
#define IS_32X 1
#define BIG_ENDIAN_DATA 1
#define ATTR_SDRAM  __attribute__((section(".data"), aligned(16)))
#define ATTR_DATA_CACHE_ALIGN __attribute__((aligned(16)))
#else
#define IS_32X 0
#define BIG_ENDIAN_DATA 0
#define ATTR_SDRAM
#define ATTR_DATA_CACHE_ALIGN
#endif

#define klabs(x)  ((x) < 0 ? -(x) : (x))
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

static inline int32_t ksgn(int32_t a) { return (a > 0) - (a < 0); }

/* -------------------------------------------------------------- multiply */
#if IS_32X
static inline int32_t mulscale16(int32_t a, int32_t b)
{
    int32_t r, hi;
    __asm__ ("dmuls.l %2,%3\n\tsts mach,%1\n\tsts macl,%0\n\txtrct %1,%0"
             : "=r"(r), "=&r"(hi) : "r"(a), "r"(b) : "mach", "macl");
    return r;
}
static inline int32_t mulscale32(int32_t a, int32_t b)
{
    int32_t hi;
    __asm__ ("dmuls.l %1,%2\n\tsts mach,%0" : "=r"(hi) : "r"(a), "r"(b) : "mach", "macl");
    return hi;
}
static inline int64_t mul64(int32_t a, int32_t b)
{
    uint32_t hi, lo;
    __asm__ ("dmuls.l %2,%3\n\tsts mach,%0\n\tsts macl,%1"
             : "=&r"(hi), "=&r"(lo) : "r"(a), "r"(b) : "mach", "macl");
    return (int64_t)(((uint64_t)hi << 32) | lo);
}
#else
static inline int32_t mulscale16(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
static inline int32_t mulscale32(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 32); }
static inline int64_t mul64(int32_t a, int32_t b) { return (int64_t)a * b; }
#endif

static inline int32_t mulscale(int32_t a, int32_t b, int n) { return (int32_t)(mul64(a, b) >> n); }
#define MS(n) static inline int32_t mulscale##n(int32_t a, int32_t b) { return (int32_t)(mul64(a, b) >> n); }
MS(1) MS(2) MS(3) MS(4) MS(5) MS(6) MS(7) MS(8) MS(9) MS(10) MS(11) MS(12) MS(13) MS(14) MS(15)
MS(17) MS(18) MS(19) MS(20) MS(21) MS(22) MS(23) MS(24) MS(25) MS(26) MS(27) MS(28) MS(29) MS(30) MS(31)
#undef MS

static inline int32_t dmulscale(int32_t a, int32_t b, int32_t c, int32_t d, int n)
{
    return (int32_t)((mul64(a, b) + mul64(c, d)) >> n);
}
#define DMS(n) static inline int32_t dmulscale##n(int32_t a, int32_t b, int32_t c, int32_t d) \
    { return (int32_t)((mul64(a, b) + mul64(c, d)) >> n); }
DMS(1) DMS(2) DMS(3) DMS(4) DMS(5) DMS(6) DMS(7) DMS(8) DMS(9) DMS(10) DMS(11) DMS(12) DMS(13) DMS(14) DMS(15) DMS(16) DMS(18) DMS(20) DMS(24) DMS(25) DMS(26) DMS(28) DMS(30) DMS(32)
#undef DMS

/* --------------------------------------------------------------- divide */
#if IS_32X
#define SH2_DVSR   (*(volatile int32_t *)0xFFFFFF00)
#define SH2_DVDNT  (*(volatile int32_t *)0xFFFFFF04)
#define SH2_DVCR   (*(volatile int32_t *)0xFFFFFF08)
#define SH2_DVDNTH (*(volatile int32_t *)0xFFFFFF10)
#define SH2_DVDNTL (*(volatile int32_t *)0xFFFFFF14)

static inline int32_t div64(int64_t num, int32_t den)
{
    if (den == 0) return num < 0 ? -0x7fffffff : 0x7fffffff;
    SH2_DVSR = den;
    SH2_DVDNTH = (int32_t)(num >> 32);
    SH2_DVDNTL = (int32_t)num;
    return SH2_DVDNTL;   /* CPU stalls until the 39-cycle divide completes */
}
static inline int32_t div32(int32_t num, int32_t den)
{
    if (den == 0) return num < 0 ? -0x7fffffff : 0x7fffffff;
    SH2_DVSR = den;
    SH2_DVDNT = num;
    return SH2_DVDNT;
}
#else
static inline int32_t div64(int64_t num, int32_t den)
{
    if (den == 0) return num < 0 ? -0x7fffffff : 0x7fffffff;
    int64_t q = num / den;
    if (q > 0x7fffffff) q = 0x7fffffff;
    if (q < -0x7fffffff - 1) q = -0x7fffffff - 1;
    return (int32_t)q;
}
static inline int32_t div32(int32_t num, int32_t den)
{
    if (den == 0) return num < 0 ? -0x7fffffff : 0x7fffffff;
    return num / den;
}
#endif

static inline int32_t divscale(int32_t a, int32_t b, int n) { return div64((int64_t)a << n, b); }
#define DVS(n) static inline int32_t divscale##n(int32_t a, int32_t b) { return div64((int64_t)a << n, b); }
DVS(1) DVS(2) DVS(3) DVS(4) DVS(5) DVS(6) DVS(8) DVS(10) DVS(12) DVS(13) DVS(14) DVS(15) DVS(16)
DVS(17) DVS(18) DVS(19) DVS(20) DVS(21) DVS(22) DVS(23) DVS(24) DVS(25) DVS(26) DVS(27) DVS(28) DVS(29) DVS(30) DVS(31) DVS(32)
#undef DVS

/* scale(a,b,c) = a*b/c with 64-bit intermediate */
static inline int32_t scale(int32_t a, int32_t b, int32_t c) { return div64(mul64(a, b), c); }

/* ----------------------------------------------------- data endianness */
/* The ROM blob is native-endian for each target, so no swapping needed.  */

#endif
