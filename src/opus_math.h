#pragma once

/*
 * The floating-point functions the codec needs, without a C runtime:
 * square roots from the SSE instruction, the rest from range reduction
 * and short polynomials (accurate to a few units in the last place of a
 * float, which is all audio needs).
 */

float om_sqrt(float x);
/* 2^x */
float om_exp2(float x);
/* log2(x) for x > 0 */
float om_log2(float x);
/* The angle of (x, y), in radians. */
float om_atan2(float y, float x);
/* Double precision sine and cosine, for tables computed once. */
double om_sin(double x);
double om_cos(double x);

#define OM_PI 3.14159265358979323846
