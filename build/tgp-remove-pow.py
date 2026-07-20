"""Remove the only pow() call on the terrain-generation path.

pow(p, (double)i) is called with i as an ordinary loop counter running 0..5, so
it is integer exponentiation dressed up as a floating-point library call.
Replacing it with repeated multiplication is mathematically identical and
faster - but the real reason is that pow() is the one libm routine proven to be
miscompiled in this toolchain's SOFT-FLOAT build (pow(0.7,3) returned 0.0).

Dropping the dependency is what makes a -msoft-float build worth testing at all,
and a soft-float build is what would let the port run on a 68LC040 or an 030
without an FPU. sin() tested correct in soft-float and is left alone.
"""
import sys

p = sys.argv[1] if len(sys.argv) > 1 else \
    "/home/angree/build/openttd-1.0.5/src/tgp.cpp"
s = open(p).read()

if "amplitude *= p" in s:
    print("already patched")
    sys.exit(0)

old = """	double total = 0.0;
	int i;

	for (i = 0; i < 6; i++) {
		const double frequency = (double)(1 << i);
		const double amplitude = pow(p, (double)i);

		total += interpolated_noise((x * frequency) / 64.0, (y * frequency) / 64.0, prime) * amplitude;
	}"""

new = """	double total = 0.0;
	double amplitude = 1.0;   /* p^0 */
	int i;

	for (i = 0; i < 6; i++) {
		const double frequency = (double)(1 << i);

		total += interpolated_noise((x * frequency) / 64.0, (y * frequency) / 64.0, prime) * amplitude;

		/* Was pow(p, (double)i). The exponent is just the loop counter, so
		 * carrying the running product is identical, cheaper, and avoids the
		 * only libm call on this path - pow() is miscompiled in this
		 * toolchain's soft-float build, which matters if we ever want to run
		 * without an FPU. */
		amplitude *= p;
	}"""

if old not in s:
    print("ANCHOR MISSING")
    sys.exit(1)

open(p, "w").write(s.replace(old, new))
print("tgp.cpp: pow() removed from perlin_coast_noise_2D")
