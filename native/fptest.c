/* fptest.c - isolate 68k double math: libnix sin/pow/basic arithmetic.
 * Prints expected vs computed. Compile at -O0/-O1/-O2 and compare.
 * Same style as native/zlibtest.c / exctest.cpp.
 */
#include <stdio.h>
#include <math.h>

static int approx(double got, double want)
{
	double d = got - want;
	if (d < 0) d = -d;
	return d < 1e-6;
}

int main(void)
{
	int fail = 0;
	printf("fptest: start (compiled with %s)\n", OPTLEVEL);

	/* basic arithmetic and conversions */
	{
		double a = 3.5, b = 2.0;
		volatile double add = a + b, mul = a * b, div = a / b;
		volatile int trunc = (int)(a * 100.0);
		volatile double fromint = (double)12345;
		printf("add=%.6f (5.5) mul=%.6f (7.0) div=%.6f (1.75) trunc=%d (350) fromint=%.1f (12345.0)\n",
			add, mul, div, trunc, fromint);
		if (!approx(add, 5.5) || !approx(mul, 7.0) || !approx(div, 1.75) || trunc != 350 || !approx(fromint, 12345.0)) fail++;
	}

	/* libm: sin at several points (tgp.cpp uses sin(x * M_PI_2)) */
	{
		volatile double s1 = sin(0.0);
		volatile double s2 = sin(M_PI_2);          /* 1.0 */
		volatile double s3 = sin(0.5 * M_PI_2);    /* 0.7071068 */
		volatile double s4 = sin(0.25 * M_PI_2);   /* 0.3826834 */
		printf("sin: %.7f (0.0) %.7f (1.0) %.7f (0.7071068) %.7f (0.3826834)\n", s1, s2, s3, s4);
		if (!approx(s1, 0.0) || !approx(s2, 1.0) || !approx(s3, 0.70710678) || !approx(s4, 0.38268343)) fail++;
	}

	/* libm: pow as used by perlin_coast_noise_2D: pow(p, i) for i=0..5 */
	{
		int i;
		double p = 0.7, expect[6] = {1.0, 0.7, 0.49, 0.343, 0.2401, 0.16807};
		for (i = 0; i < 6; i++) {
			volatile double v = pow(p, (double)i);
			printf("pow(0.7,%d)=%.6f (%.6f)%s\n", i, v, expect[i], approx(v, expect[i]) ? "" : "  <-- WRONG");
			if (!approx(v, expect[i])) fail++;
		}
	}

	/* float<->int conversions in loops like tgp's H2I and int_noise */
	{
		volatile double x = 173.25;
		volatile int ix = (int)x;
		volatile double fx = x - (double)ix;
		printf("int_conv: ix=%d (173) fx=%.2f (0.25)\n", ix, fx);
		if (ix != 173 || !approx(fx, 0.25)) fail++;
	}

	printf("fptest: %s\n", fail == 0 ? "ALL OK" : "FAILURES PRESENT");
	return 0;
}
