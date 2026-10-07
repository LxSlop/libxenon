#pragma GCC optimize ("fp-contract=off")
#define fabsf __builtin_fabsf
#define fmaf __builtin_fmaf
#include "xenos_ana_gen.h"
#include "xenos_ana_regs.h"
#include "xenos_hdmi_regs.h"
#include "xenos_ana_tables.h"
#include "ana_csc_table.h"

#define SMB(a, v)   o->smb(o->ctx, (a), (v))
#define XDVO(a, v)  o->xdvo(o->ctx, (a), (v))
#define I2C(a, v)   o->i2c(o->ctx, (a), (v))
#define DELAY(us)   o->delay_us(o->ctx, (us))

#define GPU_ANA_ENABLE   0x7900
#define GPU_ANA_SEL      0x7910
#define GPU_ANA_CTRL     0x7938

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t rotl32(uint32_t x, int n) { return n ? (x << n) | (x >> (32 - n)) : x; }
static float    bef(const uint8_t *p) { union { uint32_t u; float f; } c = { be32(p) }; return c.f; }

void ana_ksb_pll(const struct ana_ops *o, const struct ana_ksb_pll *p)
{
	uint32_t t = p->z == 5 ? 0 : p->z >> 1;
	uint32_t d0 = rotl32(rotl32((p->w << 7 | 0x80000) | t, 8) | (p->y & ~1u), 3) & ~7u;
	uint32_t v, sp = p->spread < 0 ? -p->spread : p->spread;

	SMB(ANA_KSB_CTRL, 0x62);
	SMB(ANA_PLL_D0, d0 | p->x >> 1);
	SMB(ANA_PLL_D1, p->d1hi << 16 | p->d1lo);
	v = (o->smb_rd(o->ctx, ANA_PLL_D2) & 0xffff0000) | (sp & 0x7fff) | (p->spread < 0 ? 0x8000 : 0);
	SMB(ANA_PLL_D2, v);
	SMB(ANA_KSB_CF, 0x0854acc0);
	DELAY(100);
	SMB(ANA_KSB_CTRL, o->smb_rd(o->ctx, ANA_KSB_CTRL) | 0x10);
	DELAY(100);
	SMB(ANA_KSB_CTRL, o->smb_rd(o->ctx, ANA_KSB_CTRL) & ~0x40u);
	DELAY(100);
	SMB(ANA_KSB_D3, 0x1b0a659d);
	DELAY(100);
	SMB(ANA_KSB_D3, 0x1b02659d);
	v = o->smb_rd(o->ctx, ANA_KSB_CF) | 0x40000000;
	SMB(ANA_KSB_CF, v);
	DELAY(100);
	v &= ~0x40000000u;
	SMB(ANA_KSB_CF, v);
	SMB(ANA_KSB_CF, v & ~0x08000000u);
	if (p->flag == 255)
		SMB(ANA_PLL_D0, o->smb_rd(o->ctx, ANA_PLL_D0) | 0x04000000);
}

void ana_ksb_dacs(const struct ana_ops *o)
{
	SMB(ANA_DVE_CNTL, 0);
	SMB(ANA_DAC_PWR, 0xff);
	SMB(ANA_DAC_CAL0, 0x0291028e);
	SMB(ANA_DAC_CAL1, 0x028e0285);
	SMB(ANA_DAC_CAL0, o->smb_rd(o->ctx, ANA_DAC_CAL0) | 0x80000000);
	SMB(ANA_DAC_CAL0, o->smb_rd(o->ctx, ANA_DAC_CAL0) & 0x7fffffff);
	SMB(ANA_DAC_EN, o->smb_rd(o->ctx, ANA_DAC_EN) | 0x60);
	SMB(ANA_DAC_PWR, o->smb_rd(o->ctx, ANA_DAC_PWR) | 0x80);
	DELAY(1000);
	SMB(ANA_DAC_EN, o->smb_rd(o->ctx, ANA_DAC_EN) | 0x80);
	SMB(ANA_DAC_EN, 0x1f);
}

static void ksb_from_grp(struct ana_ksb_pll *k, const uint32_t *w)
{
	union { uint32_t u; float f; } c = { w[0] };
	k->vco = c.f; k->d1lo = w[1]; k->d1hi = w[2]; k->flag = w[3]; k->x = w[4]; k->y = w[5]; k->z = w[6]; k->w = w[7]; k->spread = (int32_t)w[8];
}

static void gpu_enter(const struct ana_ops *o, uint32_t *save)
{
	*save = o->gpu_rd(o->ctx, GPU_ANA_CTRL);
	o->gpu(o->ctx, GPU_ANA_CTRL, *save & ~1u);
	o->gpu(o->ctx, GPU_ANA_SEL, 1);
	o->gpu(o->ctx, GPU_ANA_ENABLE, 1);
}

void ana_gen_tv(const struct ana_ops *o, int entry, int ver, int bl)
{
	const struct ana_tv_entry *e = &ana_tv_entries[entry];
	const uint32_t *smb = ana_grp_smb[e->grp], *dvo = ana_grp_dvo[e->grp];
	uint32_t save;
	int i, first = 0;

	SMB(ANA_POWER, o->smb_rd(o->ctx, ANA_POWER) & ~0x40000u);
	if (ver >= ANA_VER_KSB) {
		struct ana_ksb_pll k;
		SMB(ANA_DVE_CNTL, 0);
		ksb_from_grp(&k, ana_grp_pll[e->grp]);
		ana_ksb_pll(o, &k);
		ana_ksb_dacs(o);
		first = 6;
	}
	for (i = first; i < 8; i++) {
		SMB(ana_smb_addr[i] & 0xff, smb[i]);
		if (ana_smb_addr[i] & ANA_DELAY_1MS)
			DELAY(1000);
	}
	SMB(ANA_DVE_CNTL, 0x60);

	gpu_enter(o, &save);
	for (i = 0; i < 17; i++) XDVO(ana_common_addr[i], ana_common_val[i]);
	for (i = 0; i < 15; i++) XDVO(ANA_LVL_A + i, ana_lvl_a[e->lvl_a[bl]][i]);
	for (i = 0; i < 15; i++) XDVO(ANA_LVL_B + i, ana_lvl_b[e->lvl_b[bl]][i]);
	for (i = 0; i < 15; i++) XDVO(ANA_LVL_B2 + i, ana_lvl_b[e->lvl_c][i]);
	for (i = 0; i < 2; i++)  XDVO(ANA_LVL_C + i, ana_lvl_c[e->lvl_d][i]);
	for (i = 0; i < 3; i++)  XDVO(ANA_LVL_D + i, ana_lvl_d[e->lvl_e[bl]][i]);
	if (ver >= ANA_VER_KSB)
		for (i = 0; i < 7; i++) XDVO(ana_wss_addr[i], ana_grp_wss[e->grp][i]);
	for (i = 0; i < 14; i++)
		XDVO(ana_dvo_addr[i], dvo[i] | (ana_dvo_addr[i] == ANA_SYNC_CNTL ? 0x300 : 0));
	o->gpu(o->ctx, GPU_ANA_CTRL, save);
	SMB(ANA_DVE_CNTL, smb[8]);
}

int ana_pixclk_class(float pixclk)
{
	int c = (int)(170.0f / pixclk);
	return c > 12 ? 12 : c;
}

/* VD_VIDEO_TIMING accessors */
#define W0(t)      be32((t) + 0)
#define H4(t)      be16((t) + 4)
#define H6(t)      be16((t) + 6)
#define W8(t)      be32((t) + 8)
#define REP(t)     ((t)[25] >> 4)
#define CLS(t)     ((t)[26])
#define INTERLACED(t) (H4(t) & 0x1000)

static uint32_t sync_cntl(const uint8_t *t, int port)
{
	uint32_t scale = port == ANA_PORT_HDMI ? 1 : 2, b24 = t[24];
	uint32_t r6 = (W8(t) << 2) & 0x03f00000;
	uint32_t v = ((((W0(t) >> 22) / REP(t)) + 1) << 12) | scale * r6;
	if (!(b24 & 0x80)) v |= 0x08;
	if (!(b24 & 0x40)) v |= 0x20;
	return v | 0xf8000750;
}

void ana_gen_dyn(const struct ana_ops *o, const uint8_t *t, int ver, int port, int dtype, int bl, const uint8_t rnd[8])
{
	uint32_t save, v, w0 = W0(t), w8 = W8(t), rep = REP(t), htot = H6(t), hs = w0 >> 22, hact = w0 & 0xfff;
	float pix = bef(t + 20);
	int i, lv = 0, cls = CLS(t);

	SMB(ANA_POWER, o->smb_rd(o->ctx, ANA_POWER) & ~0x40000u);
	if (ver >= ANA_VER_KSB) {
		struct ana_ksb_pll k; uint32_t w[9]; for (i = 0; i < 9; i++) w[i] = be32(t + 36 + 4 * i);
		ksb_from_grp(&k, w); ana_ksb_pll(o, &k);
	} else {
		SMB(ANA_PLL_PWR0, 0x60); SMB(ANA_PLL_D0, be32(t + 28)); SMB(ANA_PLL_D1, be32(t + 32)); SMB(ANA_PLL_PWR1, 0x60); DELAY(1000);
	}
	if (port == ANA_PORT_HDMI) {
		SMB(ANA_DVE_CNTL, 0x181); DELAY(5000); SMB(ANA_DVE_CNTL, 0x81);
		if (ver >= ANA_VER_KSB) { I2C(ANA_I2C_LANE_CFG0, 0x33); I2C(ANA_I2C_LANE_CFG1, 0x33); I2C(ANA_I2C_LANE_CFG2, 0x11); I2C(ANA_I2C_LANE_CFG3, 0x11); }
	} else if (ver >= ANA_VER_KSB)
		ana_ksb_dacs(o);
	else
		SMB(ANA_DAC_EN, 7);
	SMB(ANA_PIXIN_CNTL, 9);
	SMB(ANA_MODE_CLK, 0x86000000 | (uint32_t)(cls - 1) << 27 | 0xc0 | (INTERLACED(t) ? 0 : 0x10));
	SMB(ANA_DVE_CNTL, 0x60);

	gpu_enter(o, &save);
	if (ver < ANA_VER_KSB) XDVO(port == ANA_PORT_ANALOG ? ANA_DAC_EN : ANA_DAC_PWR, 7);
	if (port == ANA_PORT_ANALOG) {
		XDVO(ANA_BLACK_SETUP, 0);
		XDVO(ANA_OUT_GAIN, 0x00087400);
		XDVO(ANA_CLK_THRESH, pix < 85.0f ? 0x24900000 : 0);
	}
	XDVO(ANA_SYNC_CNTL, sync_cntl(t, port));
	XDVO(ANA_H_TIMING, ((((H4(t) & 0xfff) + hs) / rep) << 23) | ((hact / rep) << 12) | (htot / rep));
	XDVO(ANA_H_SYNC, (((htot - hs) / rep + 1) << 12) | (hs / rep + 1));
	{
		uint32_t a = (w8 >> 24) << 18, b = w8 & 0x00fc0000, r10 = a + b, r11;
		if (INTERLACED(t))
			r11 = (w8 & 0x0003f000) | (rotl32(r10, 7) & 0xffffff80) | (rotl32(w8 & 0x0003f000, 3) & 0xfffffff8) | (r10 + 0x40000);
		else
			r11 = (rotl32(r10, 7) & 0xffffff80) | (rotl32(w8, 29) & 0x7000) | (rotl32(w8, 3) & 0x38000);
		XDVO(ANA_V_TIMING, r11 | (be16(t + 16) & (ver >= ANA_VER_KSB ? 0xfff : 0x7ff)));
	}
	XDVO(ANA_V_AUX, 0);
	if (port == ANA_PORT_ANALOG) {
		if (cls >= 1 && cls <= 7) { XDVO(ANA_SRATE, ana_srate[cls]); lv = ana_vga_lvlb_idx[cls][bl]; }
		else { XDVO(ANA_SRATE, 0x02032bf2); lv = ana_vga_lvlb_idx[0][bl]; }
		for (i = 0; i < 15; i++) XDVO(ANA_LVL_A + i, ana_lvl_a[0][i]);
		for (i = 0; i < 15; i++) XDVO(ANA_LVL_B + i, ana_lvl_b[lv][i]);
		for (i = 0; i < 15; i++) XDVO(ANA_LVL_B2 + i, ana_lvl_b[lv][i]);
		for (i = 0; i < 2; i++)  XDVO(ANA_LVL_C + i, ana_lvl_c[0][i]);
		for (i = 0; i < 3; i++)  XDVO(ANA_LVL_D + i, ana_lvl_d[0][i]);
	}
	XDVO(ANA_HDMI_EN, 1);
	XDVO(ANA_HDMI_K0, be32(t + 72) & 1 ? 0x22222222 : 0x351f2e53);
	XDVO(ANA_HDMI_K1, be32(t + 72) & 1 ? 0x22222222 : 0x4400f126);
	XDVO(ANA_HDCP_AN_LO, be32(rnd + 4));
	XDVO(ANA_HDCP_AN_HI, be32(rnd));
	o->gpu(o->ctx, GPU_ANA_CTRL, save);

	if (port == ANA_PORT_ANALOG) { SMB(ANA_DVE_CNTL, 0x4f); return; }
	SMB(ANA_DVE_CNTL, 0xc1);
	I2C(ANA_I2C_CFG_LOCK, 1);
	I2C(ANA_I2C_ENABLE, 0x37);
	I2C(HDMI_TX_AVMUTE, 0x10);
	I2C(ANA_I2C_VSTART, (w8 >> 24) + ((w8 >> 18) & 0x3f));
	v = hact / rep;
	I2C(ANA_I2C_HACTIVE, v & 0xff);
	I2C(ANA_I2C_HACTIVE+1, v >> 8 & 0xff);
	I2C(ANA_I2C_VID_ID, t[11]);
	I2C(ANA_I2C_VID_ID2, w8 >> 8 & 0xf);
	I2C(ANA_I2C_PIXREP, rep == 2 ? 0x40 : rep == 4 ? 0x60 : 0x20);
	I2C(ANA_I2C_CLK_RANGE, pix >= 100.0f ? 27 : 9);
	v = 0x4000 | (!(t[24] & 0x80) ? 0x2000 : 0) | (!(t[24] & 0x40) ? 0x1000 : 0) | (((H4(t) & 0xfff) + hs) / rep);
	I2C(ANA_I2C_HSTART, v & 0xff);
	I2C(ANA_I2C_HSTART+1, v >> 8 & 0xff);
	I2C(ANA_I2C_0x148, 0x10);
	if (INTERLACED(t)) I2C(ANA_I2C_INTERLACE, 0);
	I2C(HDMI_TX_HDMI_MODE, dtype == 3);
	I2C(ANA_I2C_CFG_LOCK, 0);
}

int ana_vga_hsync_offset(const uint8_t *t)
{
	int cls = CLS(t), k, srate_top;
	srate_top = (cls >= 1 && cls <= 7 ? ana_srate[cls] : 0x02032bf2) >> 24;
	k = cls == 1 ? 67 : cls == 2 ? 62 : (cls == 3 || cls == 4) ? 60 : (cls >= 5 && cls <= 7) ? 59 : 57;
	return (int)(((H4(t) & 0xfff) + (W0(t) >> 22)) / REP(t)) - (k - srate_top);
}

void ana_power_down(const struct ana_ops *o, int ver)
{
	SMB(ANA_DVE_CNTL, 0);
	SMB(ANA_DAC_PWR, ver >= ANA_VER_KSB ? 0x7f : 0xff);
	o->gpu(o->ctx, 0x7900, 0);
	SMB(ANA_POWER, o->smb_rd(o->ctx, ANA_POWER) | 0x40000);
	if (ver >= ANA_VER_HDMI) I2C(ANA_I2C_ENABLE, 0x36);
}

static const uint32_t scart_rgb[12] = { 0x04b100ea, 0x02640002, 0x0d4a0417, 0x0e9e0002, 0x0c940f55, 0x04170002,
					0x02000f53, 0x0e9a0002, 0x02000377, 0x00000002, 0x02000000, 0x02be0002 };

void ana_gen_csc_tv(const struct ana_ops *o, int entry, int avpack, int bl, int ver)
{
	int na = ana_tv_entries[entry].region_mask == 0x02, hd = entry >= 32, ntsc = entry < 12, pal = entry >= 12 && entry < 24, prog = entry >= 24 && entry < 32;
	int scart = avpack == 1, cs = hd ? 2 : 1, i;
	for (i = 0; i < (int)(sizeof ana_csc_tv / sizeof ana_csc_tv[0]); i++) {
		const struct ana_csc *t = &ana_csc_tv[i];
		if (t->na == na && t->hdtv == hd && t->ntsc == ntsc && t->pal == pal && t->prog == prog && t->scart == scart && t->cs == cs && t->bl == bl && t->ver17 == (ver >= ANA_VER_HDMI)) {
			int r;
			if (scart) for (r = 0; r < 6; r++) SMB(ANA_COMMON_BASE + r, scart_rgb[6 + r]);   /* RGB-in uses scart_rgb[0..5] */
			SMB(ANA_PIXIN_CNTL, 1); SMB(ANA_OUT_RANGE, t->r0e); SMB(ANA_OUT_GAIN, t->r11); SMB(ANA_BLACK_SETUP, t->r0f);
			return;
		}
	}
}

void ana_gen_csc_vga(const struct ana_ops *o, int bl)
{
	SMB(ANA_PIXIN_CNTL, 9); SMB(ANA_OUT_RANGE, 0x1ffffc00);
	SMB(ANA_OUT_GAIN, bl == 1 ? 0x0009d800 : bl == 2 ? 0x00091800 : 0x00087400);
	SMB(ANA_BLACK_SETUP, 0x066c0000);
}

int ana_avpack(int raw)
{
	int a = (raw >> 2) & 7;
	return (a == 4 || a == 7) ? 5 : a;
}

int ana_pick_tv(int region, int raw, int w, int h, int il, int hz)
{
	int i, ap = ana_avpack(raw);
	for (i = 0; i < (int)(sizeof ana_tv_entries / sizeof ana_tv_entries[0]); i++) {
		const struct ana_tv_entry *e = &ana_tv_entries[i];
		if ((e->region_mask >> region & 1) && (e->avpack_mask >> ap & 1) && e->w == w && e->h == h && e->interlaced == il && e->hz == hz)
			return i;
	}
	return -1;
}

int ana_pick_timing(int w, int h)
{
	int i, best = -1, bt = 0;
	for (i = 0; i < 15; i++) {
		const uint8_t *t = ana_timings[i];
		int hact = be32(t) & 0xfff, vact = be32(t + 8) & 0xfff, tot = be16(t + 6);
		if (hact == w && vact == h && tot > bt) { best = i; bt = tot; }
	}
	return best;
}

const uint8_t *ana_timing_blob(int idx) { return ana_timings[idx]; }

void ana_tv_geo(int e, int *tw, int *th, int *hoff, int *voff)
{
	*tw = ana_tv_entries[e].tw; *th = ana_tv_entries[e].th; *hoff = ana_tv_entries[e].hoff; *voff = ana_tv_entries[e].voff;
}

void ana_timing_crtc(int i, int hdmi, int *tw, int *th, int *hoff, int *voff)
{
	*tw = ana_timing_geo[i].tw; *th = ana_timing_geo[i].th; *voff = ana_timing_geo[i].voff;
	*hoff = hdmi ? ana_timing_geo[i].hoff_hdmi : ana_timing_geo[i].hoff_analog;
}

uint32_t ana_dve_cntl_tv(int entry) { return ana_grp_smb[ana_tv_entries[entry].grp][8]; }

static void lane_pwr(const struct ana_ops *o, uint8_t v)
{
	I2C(ANA_I2C_LANE_PWR1, v); I2C(ANA_I2C_LANE_PWR2, v); I2C(ANA_I2C_LANE_PWR3, v); I2C(ANA_I2C_LANE_PWR0, v);
}

void ana_display_off(const struct ana_ops *o, int ver, int port)
{
	uint32_t v = o->smb_rd(o->ctx, ANA_DVE_CNTL) & ~ANA_CNTL_OUTPUTS;
	if (port == ANA_PORT_HDMI) v &= ~0x200u;
	SMB(ANA_DVE_CNTL, v);
	if (ver >= ANA_VER_KSB && port == ANA_PORT_HDMI) lane_pwr(o, 0x10);
}

void ana_display_on(const struct ana_ops *o, uint32_t cntl, int ver, int port, int avpack, int submode)
{
	if (port == ANA_PORT_ANALOG && avpack == 5 && (submode & 1)) cntl &= ~6u;
	if (port == ANA_PORT_HDMI) cntl |= 0x200;
	SMB(ANA_DVE_CNTL, cntl);
	if (ver >= ANA_VER_KSB && port == ANA_PORT_HDMI) lane_pwr(o, 0);
}

uint32_t ana_dve_sync_tv(int entry) { return ana_grp_dvo[ana_tv_entries[entry].grp][7] | 0x300; }
uint32_t ana_dve_sync_dyn(const uint8_t *vt, int port) { return sync_cntl(vt, port); }

int ana_spdif_enabled(int trinity_plus, int avpack, int digital_en, int submode)
{
	if (trinity_plus) return 1;
	if (!digital_en) return 0;
	return !(avpack == 5 && (submode & 2));
}

int ana_dac_clamped(const struct ana_dac_cfg *c)
{
	if (c->hpd) return c->analog_unclamp == 0;
	return c->digital_en == 0 || c->avpack == 4;
}

void ana_dac_request(const struct ana_ops *o, const struct ana_dac_cfg *c)
{
	static const uint8_t reg[7] = { ANA_DVE_CNTL, ANA_SYNC_CNTL, ANA_SPDIF_EN, 0, 0, 0, 0 };
	static const uint16_t lane[4] = { ANA_I2C_LANE_PWR1, ANA_I2C_LANE_PWR2, ANA_I2C_LANE_PWR3, ANA_I2C_LANE_PWR0 };
	int n = c->ver >= ANA_VER_KSB ? 7 : c->ver >= ANA_VER_HDMI ? 3 : 2, i, clamp = ana_dac_clamped(c);
	int spdif = ana_spdif_enabled(c->trinity_plus, c->avpack, c->digital_en, c->submode);

	for (i = 0; i < n; i++) {
		uint32_t v = 0;
		int on = !clamp;			/* value written as-is when set, masked when clear */
		if (i == 0) {
			v = c->cntl;
			if (c->avpack == 5 && (c->submode & 2)) v &= ~6u;
		} else if (i == 1) v = c->sync;
		else if (i == 2) { v = ANA_SPDIF_ON; on = spdif; }
		if (!on) {
			v &= i == 0 ? 0xe1 : i == 1 ? 0xffffff07 : 0;
			if (c->port == ANA_PORT_HDMI) {
				if (i == 0) v &= ~0x200u;
				if (i >= 3) v |= 0x10;
			}
		}
		if (i < 3) SMB(reg[i], v);
		else I2C(lane[i - 3], v);
	}
}
