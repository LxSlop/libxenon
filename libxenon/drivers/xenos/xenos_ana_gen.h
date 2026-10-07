/* ANA (Hana) init sequence generator -- mirrors xboxkrnl VdpInitializeStaticTiming / VdpInitializeDynamicTiming.
 * Pure logic: all hardware access goes through struct ana_ops (testable off-target). */
#ifndef XENOS_ANA_GEN_H
#define XENOS_ANA_GEN_H
#include <stdint.h>
#include "xenos_ana_regs.h"

#define ANA_PORT_ANALOG 0      /* VdpVideoPortType */
#define ANA_PORT_HDMI   1

struct ana_ops {
	void     *ctx;
	void     (*smb)(void *ctx, uint8_t reg, uint32_t val);       /* SMC SMBus write */
	uint32_t (*smb_rd)(void *ctx, uint8_t reg);
	void     (*xdvo)(void *ctx, uint8_t reg, uint32_t val);      /* GPU 0x7950/0x7954 (SMBus on GPU 0x5851) */
	void     (*i2c)(void *ctx, uint16_t reg, uint8_t val);       /* Hana HDMI TX (reg >= 0x100) */
	void     (*delay_us)(void *ctx, uint32_t us);
	void     (*gpu)(void *ctx, uint32_t off, uint32_t val);      /* GPU 0x7938 save/clear/restore, 0x7910, 0x7900 */
	uint32_t (*gpu_rd)(void *ctx, uint32_t off);
};

/* kernel VD_KSB_PLL_SETTING */
struct ana_ksb_pll { float vco; uint32_t d1lo, d1hi, flag, x, y, z, w; int32_t spread; };

/* TV table entry (0..43) -> write sequence. bl = black level 0..3 */
void ana_gen_tv(const struct ana_ops *o, int entry, int ver, int bl);

/* 76-byte big-endian VD_VIDEO_TIMING (+26 = pixclk class, +28/+32 = D0/D1 for ver<0x20, +36 = ksb pll for ver>=0x20) */
void ana_gen_dyn(const struct ana_ops *o, const uint8_t *vt, int ver, int port, int display_type, int bl, const uint8_t rnd[8]);

void ana_ksb_pll(const struct ana_ops *o, const struct ana_ksb_pll *p);
void ana_ksb_dacs(const struct ana_ops *o);
void ana_power_down(const struct ana_ops *o, int ver);   /* VdpPowerDownANA (GPU 0x6028 handled by caller) */

/* pixel clock class (VGA): min(170 / pixclk_MHz, 12) */
int ana_pixclk_class(float pixclk_mhz);
/* CRTC horizontal sync offset the kernel uses for an analog-port VGA mode, before the "-1" */
int ana_vga_hsync_offset(const uint8_t *vt);

/* Hana (DVE<0x20) video PLL, see xenos_ana_pll.c */
int ana_calc_vid_pll(float *pix, uint32_t K, uint32_t *d0, uint32_t *d1, uint32_t mode);
int ana_calc_video_clocks(uint8_t *vt, int display_type);

/* Corona (DVE>=0x20) PLL, see xenos_ana_ksb.c */
int ana_calc_ksb_clocks(uint8_t *vt, int display_type, int port);

/* VdpInitializeColorSpaceConversion (analog port): black level / output range regs 0x0d,0x0e,0x0f,0x11 (+0x20..0x25 on SCART).
 * TV entry flavours derive from the entry index; cs: 1 = SD (BT.601), 2 = HD (BT.709) */
void ana_gen_csc_tv(const struct ana_ops *o, int entry, int avpack, int bl, int ver);
void ana_gen_csc_vga(const struct ana_ops *o, int bl);

/* mode pickers (kernel table selection). region: 1 NTSC-M 2 NTSC-J 3 PAL50 4 PAL60; avpack_raw: SMC 0x0f response byte.
 * ana_pick_tv: entry 0..43 or -1 (h = frame lines). ana_pick_timing: index into ana_timings (15) or -1 (largest total width wins = non reduced-blanking). */
int ana_avpack(int avpack_raw);
int ana_pick_tv(int region, int avpack_raw, int w, int h, int interlaced, int hz);
int ana_pick_timing(int w, int h);
const uint8_t *ana_timing_blob(int idx);
/* CRTC geometry the kernel uses: total w/h (frame), sync offsets (hoff already -1) */
void ana_tv_geo(int entry, int *tw, int *th, int *hoff, int *voff);
void ana_timing_crtc(int idx, int hdmi_port, int *tw, int *th, int *hoff, int *voff);

#endif
