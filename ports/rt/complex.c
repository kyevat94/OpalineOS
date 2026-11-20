/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 The OpalineOS authors */
/*
 * Compiler runtime: complex multiplication and division (__mulsc3, __divdc3...).
 * The compiler calls these for "a * b" and "a / b" on complex numbers; they follow
 * C99 Annex G (infinities and NaNs give the mathematically sensible results).
 * On Linux they come from libgcc or LLVM's compiler-rt; OpalineOS has its own.
 */

double logb(double);
float logbf(float);
long double logbl(long double);
double scalbn(double, int);
float scalbnf(float, int);
long double scalbnl(long double, int);

#define ISNAN(x) __builtin_isnan(x)
#define ISINF(x) __builtin_isinf(x)
#define ISFINITE(x) __builtin_isfinite(x)

#define DEFINE(T, S, MUL, DIV, COPYSIGN, INF, FABS, FMAX, LOGB, SCALBN)                    \
    T _Complex MUL(T a, T b, T c, T d) {                                                \
        T ac = a * c, bd = b * d, ad = a * d, bc = b * c;                                \
        T x = ac - bd, y = ad + bc;                                                       \
        if (ISNAN(x) && ISNAN(y)) {                                                       \
            int again = 0;                                                                \
            if (ISINF(a) || ISINF(b)) {                                                   \
                a = COPYSIGN(ISINF(a) ? (T)1 : (T)0, a);                                  \
                b = COPYSIGN(ISINF(b) ? (T)1 : (T)0, b);                                  \
                if (ISNAN(c)) c = COPYSIGN((T)0, c);                                      \
                if (ISNAN(d)) d = COPYSIGN((T)0, d);                                      \
                again = 1;                                                                \
            }                                                                             \
            if (ISINF(c) || ISINF(d)) {                                                   \
                c = COPYSIGN(ISINF(c) ? (T)1 : (T)0, c);                                  \
                d = COPYSIGN(ISINF(d) ? (T)1 : (T)0, d);                                  \
                if (ISNAN(a)) a = COPYSIGN((T)0, a);                                      \
                if (ISNAN(b)) b = COPYSIGN((T)0, b);                                      \
                again = 1;                                                                \
            }                                                                             \
            if (!again && (ISINF(ac) || ISINF(bd) || ISINF(ad) || ISINF(bc))) {           \
                if (ISNAN(a)) a = COPYSIGN((T)0, a);                                      \
                if (ISNAN(b)) b = COPYSIGN((T)0, b);                                      \
                if (ISNAN(c)) c = COPYSIGN((T)0, c);                                      \
                if (ISNAN(d)) d = COPYSIGN((T)0, d);                                      \
                again = 1;                                                                \
            }                                                                             \
            if (again) {                                                                  \
                x = INF * (a * c - b * d);                                                \
                y = INF * (a * d + b * c);                                                \
            }                                                                             \
        }                                                                                 \
        T _Complex z;                                                                     \
        __real__ z = x;                                                                   \
        __imag__ z = y;                                                                   \
        return z;                                                                         \
    }                                                                                     \
    T _Complex DIV(T a, T b, T c, T d) {                                                \
        int scale = 0;                                                                    \
        T lw = LOGB(FMAX(FABS(c), FABS(d)));                                              \
        if (ISFINITE(lw)) {                                                               \
            scale = (int)lw;                                                              \
            c = SCALBN(c, -scale);                                                        \
            d = SCALBN(d, -scale);                                                        \
        }                                                                                 \
        T denom = c * c + d * d;                                                          \
        T x = SCALBN((a * c + b * d) / denom, -scale);                                    \
        T y = SCALBN((b * c - a * d) / denom, -scale);                                    \
        if (ISNAN(x) && ISNAN(y)) {                                                       \
            if (denom == (T)0 && (!ISNAN(a) || !ISNAN(b))) {                              \
                x = COPYSIGN(INF, c) * a;                                                 \
                y = COPYSIGN(INF, c) * b;                                                 \
            } else if ((ISINF(a) || ISINF(b)) && ISFINITE(c) && ISFINITE(d)) {            \
                a = COPYSIGN(ISINF(a) ? (T)1 : (T)0, a);                                  \
                b = COPYSIGN(ISINF(b) ? (T)1 : (T)0, b);                                  \
                x = INF * (a * c + b * d);                                                \
                y = INF * (b * c - a * d);                                                \
            } else if (ISINF(lw) && lw > (T)0 && ISFINITE(a) && ISFINITE(b)) {            \
                c = COPYSIGN(ISINF(c) ? (T)1 : (T)0, c);                                  \
                d = COPYSIGN(ISINF(d) ? (T)1 : (T)0, d);                                  \
                x = (T)0 * (a * c + b * d);                                               \
                y = (T)0 * (b * c - a * d);                                               \
            }                                                                             \
        }                                                                                 \
        T _Complex z;                                                                     \
        __real__ z = x;                                                                   \
        __imag__ z = y;                                                                   \
        return z;                                                                         \
    }

DEFINE(float, f, __mulsc3, __divsc3, __builtin_copysignf, __builtin_inff(), __builtin_fabsf, __builtin_fmaxf,
       logbf, scalbnf)
DEFINE(double, , __muldc3, __divdc3, __builtin_copysign, __builtin_inf(), __builtin_fabs, __builtin_fmax, logb, scalbn)
DEFINE(long double, l, __mulxc3, __divxc3, __builtin_copysignl, __builtin_infl(), __builtin_fabsl, __builtin_fmaxl,
       logbl, scalbnl)
