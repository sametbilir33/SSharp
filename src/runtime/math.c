#include <math.h>

#include "runtime/math.h"

double runtime_math_abs(double value)
{
    return fabs(value);
}

double runtime_math_max(double left, double right)
{
    return fmax(left, right);
}

double runtime_math_min(double left, double right)
{
    return fmin(left, right);
}

double runtime_math_pow(double base, double exponent)
{
    return pow(base, exponent);
}

double runtime_math_sqrt(double value)
{
    return sqrt(value);
}

double runtime_math_round(double value)
{
    return round(value);
}

double runtime_math_floor(double value)
{
    return floor(value);
}

double runtime_math_ceil(double value)
{
    return ceil(value);
}

double runtime_math_truncate(double value)
{
    return trunc(value);
}

double runtime_math_sin(double value)
{
    return sin(value);
}

double runtime_math_cos(double value)
{
    return cos(value);
}

double runtime_math_tan(double value)
{
    return tan(value);
}

double runtime_math_asin(double value)
{
    return asin(value);
}

double runtime_math_acos(double value)
{
    return acos(value);
}

double runtime_math_atan(double value)
{
    return atan(value);
}

double runtime_math_log(double value)
{
    return log(value);
}

double runtime_math_log10(double value)
{
    return log10(value);
}

double runtime_math_exp(double value)
{
    return exp(value);
}

double runtime_math_sign(double value)
{
    if (value > 0.0) {
        return 1.0;
    }
    if (value < 0.0) {
        return -1.0;
    }
    return 0.0;
}

double runtime_math_clamp(double value, double min_value, double max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}
