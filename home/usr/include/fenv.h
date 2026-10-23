/* fenv.h - floating-point environment (x87 status kept clean).
 *
 * TanjaOS runs user code with a fresh x87 environment; the round
 * mode is FE_TONEAREST and all exception flags are cleared at
 * program start.  These accessors are kernel exports.
 */
#ifndef _FENV_H
#define _FENV_H

typedef struct { unsigned short cw; unsigned short sw; } fenv_t;
typedef unsigned int fexcept_t;

#define FE_INVALID    0x01
#define FE_DENORMAL   0x02
#define FE_DIVBYZERO  0x04
#define FE_OVERFLOW   0x08
#define FE_UNDERFLOW  0x10
#define FE_INEXACT    0x20
#define FE_ALL_EXCEPT (FE_INVALID | FE_DENORMAL | FE_DIVBYZERO | \
                       FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

#define FE_TONEAREST  0x0000
#define FE_DOWNWARD   0x0400
#define FE_UPWARD     0x0800
#define FE_TOWARDZERO 0x0C00

#define FE_DFL_ENV ((const fenv_t *)0)

int feclearexcept(int excepts);
int feraiseexcept(int excepts);
int fetestexcept(int excepts);
int fegetround(void);
int fesetround(int mode);
int fegetenv(fenv_t *envp);
int fesetenv(const fenv_t *envp);
int feholdexcept(fenv_t *envp);
int feupdateenv(const fenv_t *envp);

#endif /* _FENV_H */
