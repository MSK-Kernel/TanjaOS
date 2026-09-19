/* math.h - standard floating point surface.
 * NOTE: TanjaOS kernel exports do not include FPU math; these
 * declarations are here so tcc-compiled code type-checks, but
 * calling them in a user program fails to link. */
#ifndef _MATH_H
#define _MATH_H
#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))
double sin(double);  double cos(double);   double tan(double);
double asin(double); double acos(double);  double atan(double);
double atan2(double, double);
double sinh(double); double cosh(double);  double tanh(double);
double exp(double);  double log(double);   double log10(double);
double pow(double, double);
double sqrt(double); double fabs(double);
double floor(double); double ceil(double);
double fmod(double, double);
double ldexp(double, int);
double modf(double, double*);
long double ldexpl(long double x, int exp);
#endif
