/* Corona/Winchester (DVE >= 0x20) video PLL search, bit-exact port of xboxkrnl VdpCalculateKsbVideoClocks/VdpCalculateKsbDividers.
 * VCO = 900..1800 MHz, fractional divider num/den from a continued-fraction search; see ana_ksb_pll() for register packing. */
#pragma GCC optimize ("fp-contract=off")
#define fabsf __builtin_fabsf
#define fmaf __builtin_fmaf
#include <stdlib.h>
#include <stdint.h>
#include "xenos_ana_gen.h"

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static float getf(const uint8_t *p) { union { uint32_t u; float f; } c = { (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3] }; return c.f; }
static void putf(uint8_t *p, float f) { union { uint32_t u; float f; } c = { 0 }; c.f = f; put32(p, c.u); }

/* ratio = f1 / 100 / X as a rational *num / *den with den <= maxden; returns error in ppm (signed) */
static int ksb_dividers(float f1, uint32_t X, int32_t maxden, uint32_t *num, uint32_t *den)
{
	float f12, f0;
	int32_t a, h = 1, h1 = 0, k = 0, k1 = 1;
	int32_t hp = 1, kp = 0;           /* h = r10, k = r11 ; previous h1 = r4, k1 = r9 */
	int32_t r10 = 1, r11 = 0, r4 = 0, r9 = 1, r3, r8, r5 = maxden, m, hs, ks, err = 0;
	float e1;

	(void)h; (void)h1; (void)k; (void)k1; (void)hp; (void)kp;
	if (X == 0) X = 1;
	f12 = (f1 * 0.01f) / (float)X;
	a = (int32_t)f12;
	f0 = f12;
	if (r5 >= 1) {
		for (;;) {
			float fa;
			r8 = a;
			r3 = r8 * r10 + r4;
			r8 = r8 * r11;
			r4 = r10; r10 = r3;
			r3 = r8 + r9; r9 = r11; r11 = r3;
			fa = (float)a;
			if (f0 < (double)fa + 1e-6) {
				if (f0 > (double)fa - 1e-6) break;
			}
			f0 = f0 - fa;
			f0 = 1.0f / f0;
			if (f0 > 2147483648.0f) break;
			a = (int32_t)f0;
			if (r11 * a + r9 > r5) break;
		}
	}
	m = (r5 - r9) / r11;
	hs = m * r10 + r4;
	ks = m * r11 + r9;
	*num = r10; *den = r11;
	e1 = f12 - (float)r10 / (float)r11;
	err = (int)(e1 * 1e6f);
	if (fabsf(f12 - (float)hs / (float)ks) < fabsf(e1)) {
		*num = hs; *den = ks;
		err = (int)((f12 - (float)hs / (float)ks) * 1e6f);
	}
	if (*num > 65535) return 0x7fffffff;
	return err;
}

/* t: 76-byte big-endian VD_VIDEO_TIMING, updated (+20 pixclk, +26 class, +36..+68 ksb pll). */
int ana_calc_ksb_clocks(uint8_t *t, int dtype, int port)
{
	uint32_t h4 = t[4] << 8 | t[5], h6 = t[6] << 8 | t[7], h16 = t[16] << 8 | t[17], rep = t[25] >> 4, refresh = t[18];
	uint32_t flag = 255, w = 0, A = 0, B = 0, x_best = 0, y_best = 0, z_best = 0;
	int32_t best = 0x7fffffff, found = 0, cls, last = 0x7fffffff;
	float pix, f17, f12, f16, f18, f14, rem0, vco_best = 0.0f, f31;
	int n;

	cls = dtype == 1 ? ana_pixclk_class(getf(t + 20)) & 0xff : 1;
	if (cls > 12) cls = 12;
	t[26] = cls;
	if (refresh == 60 || refresh == 24 || refresh == 30) {
		float f0 = ((float)refresh * 0.9990009665f) * (float)h16;
		f0 = (f0 * (float)h6) * 1e-6f;
		putf(t + 20, f0);
		if (h4 & 0x1000) putf(t + 20, f0 * 0.5f);
	}
	pix = getf(t + 20);
	f17 = (float)cls * pix; f12 = pix * 2.0f;
	f16 = f17 < f12 ? f17 : f12;
	f18 = f17 > f12 ? f17 : f12;
	f14 = 900.0f / f18;
	n = (int)f14;
	rem0 = fmaf(-(float)n, f18, 900.0f);

	for (;;) {
		f31 = rem0 == 0.0f ? 900.0f : (float)(int)(f14 + 1.0f) * f18;
		while (f31 <= 1800.0f) {
			float f29 = 1.0f / f16, rem, d;
			uint32_t x, wv;
			rem = fmaf(-(float)(int32_t)(f29 * f31), f16, f31);
			d = fabsf(rem - f16);
			if (rem > 1.0f && d > 1.0f) { f31 += f18; continue; }
			wv = (uint32_t)fmaf(f31, 0.04f, 0.5f);
			if (wv < 9) return 0x7fffffff;           /* kernel would spin forever; unreachable for VCO >= 900 */
			for (x = 0; x <= 30; x += 2) {
				float f0 = f31 / (float)(x ? x : 1), f13;
				uint32_t y, z, a, b, k1 = 1, k2 = 1, hi, lo, Av, Bv;
				int r18;
				if (f0 < 250.0f || f0 > 1485.0f) continue;
				if (port == ANA_PORT_HDMI) {
					f13 = f17 * 10.0f;
					if (f0 / f13 < 0.99999f) continue;
					if (f0 / f13 > 1.00001f) continue;
				}
				y = (uint32_t)(f31 / f17 + 0.5f); z = (uint32_t)(f31 / f12 + 0.5f);
				if (y & 1) continue;
				if (z != 5 && (z & 1)) continue;
				r18 = ksb_dividers(f31, x, (int32_t)flag, &a, &b);
				last = r18;
				if (a < 9) k1 = (uint32_t)(9.0f / (float)a + 0.99f);
				if (b < 100) k2 = (uint32_t)(100.0f / (float)b + 0.99f);
				hi = k1 > k2 ? k1 : k2; lo = k1 > k2 ? k2 : k1;
				Av = hi * a; Bv = hi * b; (void)lo;
				if (Av <= 65535 && Bv <= flag && r18 < best) {
					vco_best = f31; w = wv; A = Av; B = Bv; x_best = x; y_best = y; z_best = z; best = r18;
				}
			}
			f31 += f18;
		}
		if (vco_best == 0.0f) {
			if (flag != 255) return 0x7fffffff;
			flag = 0xffff;
			continue;
		}
		found = 1; break;
	}
	(void)found;
	putf(t + 36, vco_best); put32(t + 40, A); put32(t + 44, B); put32(t + 48, flag);
	put32(t + 52, x_best); put32(t + 56, y_best); put32(t + 60, z_best); put32(t + 64, w);
	{
		float f11 = (float)z_best * 2.0f, f0 = (float)w * 25.0f, newpix;
		newpix = (vco_best / f11) * (float)rep;
		f0 = f0 / vco_best;
		put32(t + 68, (uint32_t)(int32_t)((1.0f - f0) * 524288.0f));
		putf(t + 20, newpix);
	}
	return last;   /* kernel returns the last evaluated divider error, not the best */
}
