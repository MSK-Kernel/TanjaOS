/* math.h - standard floating point surface.
 * The full math library (double + float + long double tiers) is
 * implemented by the kernel C library (x87/FPU) and resolved by
 * the ELF loader at exec time. */
#ifndef _MATH_H
#define _MATH_H
/* Hex-float literals that exceed the format's max exponent fold
 * to +inf at compile time (verified: tcc folds these warning-free,
 * even in static initializers).  2^1024 > double max 2^1023-eps. */
#define HUGE_VAL  0x1p+1024
#define HUGE_VALF 0x1p+128f
#define HUGE_VALL 0x1p+16384L
#define INFINITY  HUGE_VALF
/* runtime expression: the only warning-free NaN form tcc supports */
#define NAN       (0.0f/0.0f)
#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2

/* --- classification macros (portable C; tcc has no fp builtins) --- */
#define isnan(x)          ((x) != (x))
#define signbit(x)        (copysign(1.0, (x)) < 0.0)
#define isgreater(x,y)      ((x) > (y))
#define isgreaterequal(x,y) ((x) >= (y))
#define isless(x,y)         ((x) < (y))
#define islessequal(x,y)    ((x) <= (y))
#define islessgreater(x,y)  ((x) < (y) || (x) > (y))
#define isunordered(x,y)    (isnan(x) || isnan(y))
/* DBL_MAX/FLT_MAX/LDBL_MAX as hex floats (the largest finite value
 * of each format; anything bigger is inf) */
#define isinf(x) _Generic((x) + 0, \
    float:        fabsf((float)(x)) > 0x1.fffffep+127f, \
    long double:  fabsl((long double)(x)) > 0x1.fffffffffffffffp+16383L, \
    default:      fabs((double)(x)) > 0x1.fffffffffffffp+1023)
#define isfinite(x) (!isnan(x) && !isinf(x))
#define isnormal(x) (isfinite(x) && _Generic((x) + 0, \
    float:        fabsf((float)(x)) >= 0x1p-126f, \
    long double:  fabsl((long double)(x)) >= 0x1p-16382L, \
    default:      fabs((double)(x)) >= 0x1p-1022))
#define fpclassify(x) (isnan(x) ? FP_NAN : isinf(x) ? FP_INFINITE : \
    _Generic((x) + 0, float: fabsf((float)(x)), \
             long double: fabsl((long double)(x)), \
             default: fabs((double)(x))) == 0.0 ? FP_ZERO : \
    isnormal(x) ? FP_NORMAL : FP_SUBNORMAL)
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4

/* --- double tier --- */
double sin(double);    double cos(double);    double tan(double);
double asin(double);    double acos(double);   double atan(double);
double atan2(double, double);
double sinh(double);    double cosh(double);   double tanh(double);
double asinh(double);   double acosh(double);  double atanh(double);
double exp(double);     double exp2(double);   double expm1(double);
double log(double);     double log2(double);   double log10(double);
double log1p(double);   double logb(double);
double pow(double, double);
double sqrt(double);    double cbrt(double);
double fabs(double);    double hypot(double, double);
double floor(double);   double ceil(double);
double trunc(double);   double round(double);
double rint(double);    double nearbyint(double);
long   lround(double);  long long llround(double);
long   lrint(double);   long long llrint(double);
double fmod(double, double);
double remainder(double, double);
double remquo(double, double, int*);
double modf(double, double*);
double frexp(double, int*);
double ldexp(double, int);
double scalbn(double, int);
double scalbln(double, int);
int    ilogb(double);
double copysign(double, double);
double nextafter(double, double);
double fdim(double, double);
double fmax(double, double);
double fmin(double, double);
double fma(double, double, double);
double erf(double);     double erfc(double);
double tgamma(double);  double lgamma(double);

/* --- float tier --- */
float sinf(float);      float cosf(float);      float tanf(float);
float asinf(float);     float acosf(float);     float atanf(float);
float atan2f(float, float);
float sinhf(float);     float coshf(float);     float tanhf(float);
float asinhf(float);    float acoshf(float);    float atanhf(float);
float expf(float);      float exp2f(float);     float expm1f(float);
float logf(float);      float log2f(float);     float log10f(float);
float log1pf(float);    float logbf(float);
float powf(float, float);
float sqrtf(float);     float cbrtf(float);
float fabsf(float);     float hypotf(float, float);
float floorf(float);   float ceilf(float);
float truncf(float);    float roundf(float);
float rintf(float);    float nearbyintf(float);
long   lroundf(float);  long long llroundf(float);
long   lrintf(float);   long long llrintf(float);
float fmodf(float, float);
float remainderf(float, float);
float remquof(float, float, int*);
float modff(float, float*);
float frexpf(float, int*);
float ldexpf(float, int);
float scalbnf(float, int);
float scalblnf(float, int);
int    ilogbf(float);
float copysignf(float, float);
float nextafterf(float, float);
float fdimf(float, float);
float fmaxf(float, float);
float fminf(float, float);
float fmaf(float, float, float);
float erff(float);      float erfcf(float);
float tgammaf(float);   float lgammaf(float);

/* --- long double tier (mapped onto double) --- */
long double sinl(long double);      long double cosl(long double);
long double tanl(long double);
long double asinl(long double);     long double acosl(long double);
long double atanl(long double);
long double atan2l(long double, long double);
long double sinhl(long double);     long double coshl(long double);
long double tanhl(long double);
long double asinhl(long double);    long double acoshl(long double);
long double atanhl(long double);
long double expl(long double);      long double exp2l(long double);
long double expm1l(long double);
long double logl(long double);      long double log2l(long double);
long double log10l(long double);
long double log1pl(long double);    long double logbl(long double);
long double powl(long double, long double);
long double sqrtl(long double);    long double cbrtl(long double);
long double fabsl(long double);    long double hypotl(long double, long double);
long double floorl(long double);  long double ceill(long double);
long double truncl(long double);  long double roundl(long double);
long double rintl(long double);   long double nearbyintl(long double);
long   lroundl(long double);     long long llroundl(long double);
long   lrintl(long double);      long long llrintl(long double);
long double fmodl(long double, long double);
long double remainderl(long double, long double);
long double remquol(long double, long double, int*);
long double modfl(long double, long double*);
long double frexpl(long double, int*);
long double ldexpl(long double, int);
long double scalbnl(long double, int);
long double scalblnl(long double, int);
int    ilogbl(long double);
long double copysignl(long double, long double);
long double nextafterl(long double, long double);
long double fdiml(long double, long double);
long double fmaxl(long double, long double);
long double fminl(long double, long double);
long double fmal(long double, long double, long double);
long double erfl(long double);     long double erfcl(long double);
long double tgammal(long double);  long double lgammal(long double);

#endif
