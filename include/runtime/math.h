#ifndef SSHARP_RUNTIME_MATH_H
#define SSHARP_RUNTIME_MATH_H

#include <stddef.h>

double runtime_math_abs(double value);
double runtime_math_max(double left, double right);
double runtime_math_min(double left, double right);
double runtime_math_pow(double base, double exponent);
double runtime_math_sqrt(double value);
double runtime_math_round(double value);
double runtime_math_floor(double value);
double runtime_math_ceil(double value);
double runtime_math_truncate(double value);
double runtime_math_sin(double value);
double runtime_math_cos(double value);
double runtime_math_tan(double value);
double runtime_math_asin(double value);
double runtime_math_acos(double value);
double runtime_math_atan(double value);
double runtime_math_log(double value);
double runtime_math_log10(double value);
double runtime_math_exp(double value);
double runtime_math_sign(double value);
double runtime_math_clamp(double value, double min_value, double max_value);

#endif
