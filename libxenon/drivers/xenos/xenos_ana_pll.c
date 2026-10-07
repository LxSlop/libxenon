/* Hana (DVE < 0x20) video PLL search, bit-exact port of xboxkrnl VdpCalculateAnaVidPLL.
 * VCO = 27 MHz * N * b / M  (520..1040 MHz); out = VCO / d.
 * Packed word (ANA D0/D1): ((((d-1) * 32 + b-1) * 32 + M-1) * 64 + N-1) */
#pragma GCC optimize ("fp-contract=off")
#define fabsf __builtin_fabsf
#define fmaf __builtin_fmaf
#include <stdlib.h>
#include <stdint.h>
#include "xenos_ana_gen.h"

struct sol { float f; uint32_t code; };

static void sort_sols(struct sol *a, unsigned n, float target)
{
	unsigned i, swapped;
	if (n < 2) return;
	do {
		swapped = 0;
		for (i = 0; i + 1 < n; i++) {
			float da = fabsf(target - a[i].f), db = fabsf(target - a[i + 1].f);
			if (da > db || (da == db && a[i].f > a[i + 1].f)) {
				struct sol t = a[i]; a[i] = a[i + 1]; a[i + 1] = t; swapped = 1;
			}
		}
	} while (swapped);
}

static void store_best(struct sol *a, unsigned *cnt, float target, uint32_t N, uint32_t b, uint32_t M, uint32_t d)
{
	uint32_t code = ((((d - 1) << 5) + b - 1) << 5) + M - 1;
	float fN = (float)N, fb = (float)b, f5;
	unsigned i;
	code = (code << 6) + N - 1;
	f5 = ((fb * fN) * 27.0f) / (float)(M * d);

	for (i = 0; i < *cnt; i++) {
		double thr = (double)f5 * 1e-5;
		if ((double)fabsf(a[i].f - f5) < thr) {   /* near-duplicate frequency: keep the "better" divider set */
			float nn = (float)N, bb = fb, mm = (float)(M * M);
			float merit_new = fabsf((((bb * nn) * nn * 729.0f) / mm - 62500.0f) * 100.0f);
			uint32_t c = a[i].code, N2 = (c & 63) + 1, b2 = ((c >> 11) & 31) + 1, M2 = ((c >> 6) & 31) + 1;
			float n2 = (float)N2, b2f = (float)b2;
			float merit_old = fabsf((((n2 * n2) * b2f * 729.0f) / (float)(M2 * M2) - 62500.0f) * 100.0f);
			if (merit_new < merit_old) { a[i].f = f5; a[i].code = code; }
			else if (merit_new == merit_old) {
				float x = (fb / (float)M + 1.0f) * fN, y = (b2f / (float)M2 + 1.0f) * n2;
				if (x * 27.0f < y * 27.0f) { a[i].f = f5; a[i].code = code; }
			}
			return;
		}
	}
	a[*cnt].f = f5; a[*cnt].code = code; (*cnt)++;
	if (*cnt >= 32) { sort_sols(a, 32, target); (*cnt)--; }
}

/* mode 0: nearest, 1: only >= target, 2: only <= target. pix is in/out (MHz). returns error in ppm, or -1 if no solution */
int ana_calc_vid_pll(float *pix, uint32_t K, uint32_t *d0, uint32_t *d1, uint32_t mode)
{
	struct sol A[33], B[33];
	unsigned cA = 0, cB = 0, i, j;
	float f2 = *pix * 2.0f, f31 = (float)K * *pix;
	uint32_t N, M, b, d;

	for (N = 2; N <= 64; N++) {
		float vco = (float)N * 27.0f;
		if (vco < 520.0f || vco > 1040.0f || N == 33 || N == 65 || N == 97) continue;
		for (M = 2; M <= 32; M++) {
			float fm = (float)M;
			if (vco < fm * 100.0f * 0.5f || vco > fm * 100.0f) continue;
			for (b = 2; b <= 32; b++) {
				float v = (float)(N * b) * 27.0f;
				if (v < fm * 1040.0f * 0.5f || v > fm * 1040.0f) continue;
				for (d = 2; d <= 128; d++) {
					float f, fa, fb_;
					int ea, eb;
					if (d == 65 || d == 66 || d == 67) continue;
					f = (((float)b * (float)N) * 27.0f) / (float)(M * d);
					if (mode == 0) {
						fa = fabsf(((f - f31) / f31) * 1e6f); fb_ = fabsf(((f - f2) / f2) * 1e6f);
						ea = (int)fa; eb = (int)fb_;
					} else {
						float da = mode == 1 ? f - f31 : f31 - f, db = mode == 1 ? f - f2 : f2 - f;
						ea = (int)((da / f31) * 1e6f); eb = (int)((db / f2) * 1e6f);
						if (ea < -10) ea = 5000;
						if (eb < -10) eb = 5000;
						ea = abs(ea); eb = abs(eb);
					}
					if (ea < 5000) store_best(A, &cA, f31, N, b, M, d);
					if (eb < 5000) store_best(B, &cB, f2, N, b, M, d);
				}
			}
		}
	}
	if (!cA || !cB) return -1;
	sort_sols(A, cA, f31);
	sort_sols(B, cB, f2);
	for (i = 0, j = 0; i < cB && j < cA; ) {
		float ra = (f31 - A[j].f) * (1.0f / f31), rb = (f2 - B[i].f) * (1.0f / f2);
		float diff = fmaf(f2 - B[i].f, 1.0f / f2, -ra);
		if ((double)fabsf(diff) < 1e-5) {
			*d0 = A[j].code; *d1 = B[i].code; *pix = B[i].f * 0.5f;
			return (int)(fabsf(rb) * 1e6f);
		}
		if (fabsf(rb) > fabsf(ra)) j++;
		else if (fabsf(rb) != fabsf(ra)) i++;
		else { float bf = B[i].f, af = A[j].f; if (f2 > bf) i++; if (f31 > af) j++; }
	}
	return -1;
}

/* VdpCalculateAnaVideoClocks: fills +26 (pixclk class), +28/+32 (D0/D1), +20 (achieved pixclk). dtype: VdpDisplayType (1=VGA, 3=HDMI). returns ppm error or -1 */
int ana_calc_video_clocks(uint8_t *t, int dtype)
{
	union { uint32_t u; float f; } c;
	uint32_t h4 = t[4] << 8 | t[5], h6 = t[6] << 8 | t[7], h16 = t[16] << 8 | t[17], rep = t[25] >> 4, il = (h4 >> 12) & 1, d0 = 0, d1 = 0;
	float pix, f;
	int ret, cls, thr;

	c.u = (uint32_t)t[20] << 24 | t[21] << 16 | t[22] << 8 | t[23]; pix = c.f;
	cls = dtype == 1 ? ana_pixclk_class(pix) & 0xff : 1;
	if (cls > 12) cls = 12;
	t[26] = cls;
	if (t[18] == 60) {
		f = (float)((60 / (il + 1)) * h6 * h16) / ((float)rep * 1001000.0f);
		ret = ana_calc_vid_pll(&f, cls, &d0, &d1, 1);
		if (dtype == 3 && ret > 10) {
			f = (float)((t[18] / (il + 1)) * h6 * h16) / ((float)rep * 1000000.0f);
			ret = ana_calc_vid_pll(&f, cls, &d0, &d1, 0);
		}
	} else {
		f = pix / (float)rep;
		ret = ana_calc_vid_pll(&f, cls, &d0, &d1, 0);
	}
	t[28] = d0 >> 24; t[29] = d0 >> 16; t[30] = d0 >> 8; t[31] = d0;
	t[32] = d1 >> 24; t[33] = d1 >> 16; t[34] = d1 >> 8; t[35] = d1;
	thr = dtype == 3 ? ((t[27] & 0x7f) ? 10 : 5000) : 5000;
	if (ret > thr) return -1;
	if (f != 0.0f) { c.f = (float)rep * f; t[20] = c.u >> 24; t[21] = c.u >> 16; t[22] = c.u >> 8; t[23] = c.u; }
	return ret;
}
