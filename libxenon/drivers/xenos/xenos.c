#include <xenos/xenos.h>

#include <xenos/xenos_edid.h>
#include <xenon_smc/xenon_smc.h>
#include <xenon_smc/xenon_gpio.h>
#include <xenon_nand/xenon_config.h>
#include <xb360/xb360.h>
#include <pci/io.h>
#include <stdio.h>
#include <stdlib.h>
#include <time/time.h>
#include <string.h>
#include <assert.h>
#include <xetypes.h>
#include "xenos_ana_gen.h"
#include "xenos_hdmi.h"
#include "gpu_csc.h"
#include <ppc/timebase.h>

#define require(x, label) if (!(x)) { printf("%s:%d [%s]\n", __FILE__, __LINE__, #x); goto label; }

#define FB_BASE 0x1e000000

/* GPU registers touched here (names from xboxkrnl usage) */
#define GPU_CRTC_CONTROL       0x6028	/* bit0 enable, bit24 reset request (kernel VdpPowerDownANA/VdpSetDisplayTimingParameters) */
#define   CRTC_CONTROL_ENABLE    0x00000001
#define   CRTC_CONTROL_RESET_REQ 0x01000000
#define   CRTC_CONTROL_HOLD      0x00000301	/* cleared during ANA power-down */
#define GPU_CRTC_RESET_STATUS  0x281c	/* bit1 = reset acknowledged */
#define   CRTC_RESET_ACK         0x2
#define GPU_ANA_PWRDOWN_HOLD   0x04a0	/* 0x100 written between power-down and ANA init (kernel) */
#define GPU_CLOCK_GATING0      0x0204
#define GPU_CLOCK_GATING1      0x0208
#define GPU_MODE_LOCK          0x60e0	/* 1 while a mode change is in progress (kernel VdSetDisplayMode) */
#define GPU_FORCE_DATA_COLOR   0x793c	/* 0x200 YCbCr, 0 RGB */
#define XENOS_GPIO_VIDEO_MASK  0x2000	/* HalGpioControl(4|5, 0x2000, 0x2000) after mode set */
#define XENOS_MODE_SWITCH_MS   350	/* kernel: >= 350 ms between ANA power-down and re-init */

u32 xenos_id = 0; // 5841=slim (trinity, corona), 5831=jasper, 5821=zephyr/falcon?, 5811=xenon?, 5851=winchester
static u32 xenos_asic = 0;	/* kernel VdpGraphicsAsicID: (device id << 16) | PCI revision */
int xenos_hdmi_sink = 0;	/* EDID says the sink is an HDMI (not DVI) monitor */
int xenos_is_corona = 0;
int xenos_black_level = 0;	/* XConfig VideoOutputBlackLevels: 0..2 */
static struct edid * xenos_edid = NULL;

struct mode_s
{
	const char *name;
	int kind;		/* XM_TV / XM_VGA / XM_HDMI */
	int ana_idx;		/* TV: kernel table entry */
	int height;		/* active lines per field/frame as scanned out */
	int total_height;
	int active_height;
	int width;		/* pitch, padded to 32 */
	int total_width;
	int real_active_width;
	int hsync_offset;
	int vsync_offset;
	int composite_sync;
	int is_progressive;
	int rgb;
	int hdmi;
	int overscan;
	uint32_t scl_tap, scl_65b8, scl_65c8;	/* display scaler constants (VdInitializeScaler, 1:1) */
	int pix_khz;		/* digital modes: achieved pixel clock */
	uint32_t sync_cntl;	/* ANA_SYNC_CNTL value of the mode (needed to re-apply DAC state) */
	int hz;
};

enum { XM_TV, XM_VGA, XM_HDMI };

static const struct { const char *name; int kind, w, h, il, hz; } xenos_mode_defs[VIDEO_MODE_COUNT] = {
	[VIDEO_MODE_PAL50]         = { "PAL50",            XM_TV,   640,  576, 1, 50 },
	[VIDEO_MODE_PAL60]         = { "PAL60",            XM_TV,   640,  480, 1, 60 },
	[VIDEO_MODE_NTSC]          = { "NTSC",             XM_TV,   640,  480, 1, 60 },
	[VIDEO_MODE_YUV_480P]      = { "Component 480p",   XM_TV,   640,  480, 0, 60 },
	[VIDEO_MODE_YUV_720P]      = { "Component 720p",   XM_TV,  1280,  720, 0, 60 },
	[VIDEO_MODE_HDMI_720P]     = { "HDMI 720p",        XM_HDMI,1280,  720, 0, 60 },
	[VIDEO_MODE_VGA_640x480]   = { "VGA 640x480",      XM_VGA,  640,  480, 0, 60 },
	[VIDEO_MODE_VGA_1024x768]  = { "VGA 1024x768",     XM_VGA, 1024,  768, 0, 60 },
	[VIDEO_MODE_VGA_1280x720]  = { "VGA 1280x720",     XM_VGA, 1280,  720, 0, 60 },
	[VIDEO_MODE_VGA_1280x768]  = { "VGA 1280x768",     XM_VGA, 1280,  768, 0, 60 },
	[VIDEO_MODE_VGA_1280x1024] = { "VGA 1280x1024",    XM_VGA, 1280, 1024, 0, 60 },
	[VIDEO_MODE_VGA_1360x768]  = { "VGA 1360x768",     XM_VGA, 1360,  768, 0, 60 },
	[VIDEO_MODE_VGA_1440x900]  = { "VGA 1440x900",     XM_VGA, 1440,  900, 0, 60 },
	[VIDEO_MODE_YUV_1080I]     = { "Component 1080i",  XM_TV,  1920, 1080, 1, 60 },
	[VIDEO_MODE_YUV_1080P]     = { "Component 1080p",  XM_TV,  1920, 1080, 0, 60 },
	[VIDEO_MODE_HDMI_1080P]    = { "HDMI 1080p",       XM_HDMI,1920, 1080, 0, 60 },
	[VIDEO_MODE_VGA_848x480]   = { "VGA 848x480",      XM_VGA,  848,  480, 0, 60 },
	[VIDEO_MODE_VGA_1680x1050] = { "VGA 1680x1050",    XM_VGA, 1680, 1050, 0, 60 },
	[VIDEO_MODE_VGA_1920x1080] = { "VGA 1920x1080",    XM_VGA, 1920, 1080, 0, 60 },
	[VIDEO_MODE_YUV_576P]      = { "Component 576p",   XM_TV,   640,  576, 0, 50 },
};

static struct mode_s xenos_mode_cur;
static int xenos_cur_id = -1;
static struct mode_s * xenos_current_mode = NULL;

void xenos_write32(int reg, uint32_t val)
{
	write32n(0xec800000 + reg, val);
//	printf("set reg %4x %08x, read %08x\n", reg, val, read32n(0x200ec800000 + reg));
}

uint32_t xenos_read32(int reg)
{
	return read32n(0xec800000 + reg);
}

/* DVE register through the GPU window (XDVO) */
static void xenos_ana_write(int addr, uint32_t reg)
{
	xenos_write32(GPU_ANA_XDVO_REG, addr);
	xenos_read32(GPU_ANA_XDVO_REG);
	xenos_write32(GPU_ANA_XDVO_VAL, reg);
	uint32_t cycle = 0;
	while (xenos_read32(GPU_ANA_XDVO_REG) == addr && cycle < 1000) {
		if (!(cycle % 250)) printf(".");
		cycle++;
	}
	if (cycle == 1000)
		printf("\nxenos_ana_write - addr: 0x%X reg: 0x%X - FAILED!\n", addr, reg);
}

static int isCoronaOrWinchester()
{
	int type = xenon_get_console_type();
	if (type >= REV_CORONA)
		return 1;
	return 0;
}

/* ---- ANA / HDMI TX access glue -------------------------------------------------------------- */
static void g_smb(void *c, uint8_t r, uint32_t v) { xenon_smc_ana_write(r, v); }
static uint32_t g_smb_rd(void *c, uint8_t r) { uint32_t v = 0; xenon_smc_ana_read(r, &v); return v; }
/* kernel VdpWriteXDVOUlong: DVE registers go over the SMC for ASIC 0x58510000 (Winchester rev 0) and Corona with eMMC; everyone else uses the GPU window */
static int xenos_xdvo_via_smc(void) { return xenos_asic == 0x58510000 || xenon_get_console_type() == REV_CORONA_PHISON; }
static void g_xdvo(void *c, uint8_t r, uint32_t v) { if (xenos_xdvo_via_smc()) xenon_smc_ana_write(r, v); else xenos_ana_write(r, v); }
static void g_i2c(void *c, uint16_t r, uint8_t v) { xenon_smc_i2c_write(r, v); }
static void g_delay(void *c, uint32_t us) { udelay(us); }
static void g_gpu(void *c, uint32_t o, uint32_t v) { xenos_write32(o, v); }
static uint32_t g_gpu_rd(void *c, uint32_t o) { return xenos_read32(o); }
static const struct ana_ops xenos_ana_ops = { 0, g_smb, g_smb_rd, g_xdvo, g_i2c, g_delay, g_gpu, g_gpu_rd };

static void h_wr(void *c, uint16_t r, uint8_t v) { xenon_smc_i2c_write(r, v); }
static uint8_t h_rd(void *c, uint16_t r) { uint8_t v = 0; xenon_smc_i2c_read(r, &v); return v; }
static const struct hdmi_ops xenos_hdmi_ops = { 0, h_wr, h_rd, g_delay };

/* DVE version: ANA_VERSION -> (v & 0xf) | ((v >> 12) & 0xf0); 0x01 old, 0x11 HDMI Hana, 0x23 Corona */
int xenos_ana_version(void)
{
	uint32_t v;
	if (xenon_smc_ana_read(ANA_VERSION, &v))
		return xenos_is_corona ? ANA_VER_KSB : ANA_VER_HDMI;
	return (v & 0xf) | ((v >> 12) & 0xf0);
}

/* HDMI audio: audio clock N follows the TMDS clock. Idempotent; needs an HDMI sink and an HDMI port mode. */
void xenos_hdmi_audio_enable(void)
{
	if (!xenos_current_mode || !xenos_current_mode->hdmi || !xenos_hdmi_sink)
		return;
	hdmi_audio_setup(&xenos_hdmi_ops, xenos_current_mode->pix_khz);
}

void xenos_init_phase0(void)
{
	xenos_write32(0xe0, 0x10000000);
	xenos_write32(0xec, 0xffff);	/* 16 bit register: kernel writes 0xffff and checks the read-back */

	xenos_write32(0x1724, 0);
	int i;
	for (i = 0; i < 8; ++i)
	{
		/* kernel VdpInitialStateRegisterConfig list (0x8014 is written twice there) */
		int addr[] = {0x8000, 0x8c60, 0x8008, 0x8004, 0x800c, 0x8010, 0x8014, 0x8014, 0x880c, 0x8800, 0x8434, 0x8820, 0x8804, 0x8824, 0x8828, 0x882c, 0x8420, 0x841c, 0x8418, 0x8414, 0x8c78, 0x8c7c, 0x8c74};
		int j;
		for (j = 0; j < 23; ++j)
			xenos_write32(addr[j] + i * 0x1000, 0);
	}
}

/* kernel VdEnableDisableClockGating(1): gating masks by ASIC. Only Jasper (5831) gets the 0x903ff006 / 0x3fc7c2 set;
 * Xenon 0x4000300, everything else (Falcon, Trinity, Corona, Winchester) 0x4400380; 0x208 = 0x180002 otherwise. */
static void xenos_clock_gating(void)
{
	uint32_t r204 = xenos_asic < 0x58210000 ? 0x4000300 : 0x4400380, r208 = 0x180002;
	if (xenos_asic - 0x58310000 <= 0xfffff) {
		r204 |= 0x903ff006;
		r208 = 0x3fc7c2;
	}
	xenos_write32(GPU_CLOCK_GATING0, r204);
	xenos_write32(GPU_CLOCK_GATING1, r208);
}

void xenos_init_phase1(void)
{
	if (xenos_id>=0x5841){
		uint32_t v,v2;
		
		v = xenos_read32(0x0218) & 0xFE7FFFFF;
		xenos_write32(0x0218,v);
		xenos_read32(0x0218);

		v2 = 0xa058a34;

		xenos_write32(0x244,v2);

		v &= 0xFFFFF803;
		v |= 0x388;
		xenos_write32(0x0218,v);
		xenos_read32(0x0218);
		udelay(1000);

		v2 |= 1;
		xenos_write32(0x244,v2);
		xenos_read32(0x244);
		udelay(1000);

		v |= 0x1000000;
		xenos_write32(0x0218,v);
		xenos_read32(0x0218);

		xenos_write32(0x0218,xenos_read32(0x0218) & 0xFFFFFFFE);
	}else{
		uint32_t v;
		xenos_write32(0x0244, 0x20000000); //
		v = 0x200c0011;
		xenos_write32(0x0244, v);
		udelay(1000);
		v &=~ 0x20000000;
		xenos_write32(0x0244, v);
		udelay(1000);

		assert(xenos_read32(0x0244) == 0xc0011);

		xenos_write32(0x0218, 0);
	}

	xenos_write32(0x3c04, 0xe);
	xenos_write32(0x00f4, 4);
	
	if (xenos_id < 0x5841) {
		int gpu_clock = xenos_read32(0x0210);
		int mem_clock = xenos_read32(0x284);
		int edram_clock = xenos_read32(0x244);
		int fsb_clock = xenos_read32(0x248);
		assert(gpu_clock == 9);
		printf("GPU Clock at %d MHz\n", 50 * ((gpu_clock & 0xfff)+1) / (((gpu_clock >> 12) & 0x3f)+1));
		printf("MEM Clock at %d MHz\n", 50 * ((mem_clock & 0xfff)+1) / (((mem_clock >> 12) & 0x3f)+1));
		printf("EDRAM Clock at %d MHz\n", 100 * ((edram_clock & 0xfff)+1) / (((edram_clock >> 12) & 0x3f)+1));
		printf("FSB Clock at %d MHz\n", 100 * ((fsb_clock & 0xfff)+1) / (((fsb_clock >> 12) & 0x3f)+1));

	}
	xenos_clock_gating();
	
	xenos_write32(0x01a8, 0);
	xenos_write32(0x0e6c, 0x0c0f0000);
	xenos_write32(0x3400, 0x40401);
	udelay(1000);
	xenos_write32(0x3400, 0x40400);
	xenos_write32(0x3300, 0x3a22);
	xenos_write32(0x340c, 0x1003f1f);
	xenos_write32(0x00f4, 0x1e);
	
	
	xenos_write32(0x2800, 0);
	xenos_write32(0x2804, 0x20000000);
	xenos_write32(0x2808, 0x20000000);
	xenos_write32(0x280c, 0);
	xenos_write32(0x2810, 0x20000000);
	xenos_write32(0x2814, 0);

	udelay(1000);	
	xenos_write32(0x6548, 0);
	
		// resetgui
		// initcp
		
	xenos_write32(0x04f0, (xenos_read32(0x04f0) &~ 0xf0000) | 0x40100);
}


/* kernel VdpTurnDisplayOffInternal: outputs off for the port of the mode being left */
static void xenos_ana_stop_display(void)
{
	ana_display_off(&xenos_ana_ops, xenos_ana_version(), xenos_current_mode && xenos_current_mode->hdmi);
}

/* kernel VdpPowerDownANA */
void xenos_ana_preinit(void)
{
	xenos_ana_stop_display();
	uint32_t v;
	int ver = xenos_ana_version();
	if (xenos_current_mode)
		udelay(XENOS_MODE_SWITCH_MS * 1000);
	xenos_write32(GPU_CRTC_CONTROL, v = (xenos_read32(GPU_CRTC_CONTROL) | CRTC_CONTROL_RESET_REQ));
	while (!(xenos_read32(GPU_CRTC_RESET_STATUS) & CRTC_RESET_ACK));
	xenon_smc_ana_write(ANA_DVE_CNTL, 0);
	xenon_smc_ana_write(ANA_DAC_PWR, ver >= ANA_VER_KSB ? 0x7f : 0xff);
	xenos_write32(GPU_CRTC_CONTROL, v & ~CRTC_CONTROL_HOLD);
	xenos_write32(GPU_ANA_ENABLE, 0);
	xenon_smc_ana_read(ANA_POWER, &v);
	xenon_smc_ana_write(ANA_POWER, v | 0x40000);
	if (ver >= ANA_VER_HDMI)
		xenon_smc_i2c_write(ANA_I2C_ENABLE, 0x36);
}

void xenos_set_mode_f1(struct mode_s *mode)
{
	xenos_write32(D1CRTC_UPDATE_LOCK, 1);
	xenos_write32(DCP_LB_DATA_GAP_BETWEEN_CHUNK, 3);
	xenos_write32(D1CRTC_DOUBLE_BUFFER_CONTROL, 0);
	xenos_write32(D1CRTC_V_TOTAL, 0);

	int interlace_factor = mode->is_progressive ? 1 : 2;

	xenos_write32(D1CRTC_H_TOTAL, mode->total_width - 1);
	xenos_write32(D1CRTC_H_SYNC_B, mode->total_height - 1);
	xenos_write32(D1CRTC_H_BLANK_START_END, (mode->hsync_offset << 16) | (mode->real_active_width + mode->hsync_offset));
	xenos_write32(D1CRTC_H_SYNC_B_CNTL, (mode->vsync_offset << 16) | (mode->active_height * interlace_factor + mode->vsync_offset));
	xenos_write32(D1CRTC_H_SYNC_A, 0x100000);
	xenos_write32(0x6018, 0x60000);
	xenos_write32(D1CRTC_V_SYNC_B, mode->is_progressive ? 0 : 1);
	xenos_write32(D1CRTC_H_SYNC_A_CNTL, 0);
	xenos_write32(0x601c, 0);

	xenos_write32(D1CRTC_MVP_INBAND_CNTL_CAP, 0);
	xenos_write32(D1CRTC_MVP_INBAND_CNTL_INSERT, 0);
	xenos_write32(D1CRTC_MVP_FIFO_STATUS, 0);
	xenos_write32(D1CRTC_MVP_SLAVE_STATUS, 0);

	xenos_write32(D1CRTC_UPDATE_LOCK, 0);
}


void xenos_set_mode_f2(struct mode_s *mode)
{
	int interlace_factor = mode->is_progressive ? 1 : 2;

	xenos_write32(D1GRPH_UPDATE, 1);
	xenos_write32(D1GRPH_PITCH, mode->width); /* pitch */
	xenos_write32(D1GRPH_CONTROL, 2);
	xenos_write32(D1GRPH_LUT_SEL, 0); /* lut override */
	xenos_write32(D1GRPH_SURFACE_OFFSET_X, 0);
	xenos_write32(D1GRPH_SURFACE_OFFSET_Y, 0);
	xenos_write32(D1GRPH_X_START, 0);
	xenos_write32(D1GRPH_Y_START, 0);
	xenos_write32(D1GRPH_X_END, mode->width);
	xenos_write32(D1GRPH_Y_END, mode->active_height * interlace_factor);
	xenos_write32(D1GRPH_PRIMARY_SURFACE_ADDRESS, FB_BASE);
	xenos_write32(D1GRPH_PITCH, mode->width);
	xenos_write32(D1GRPH_ENABLE, 1);
	xenos_write32(D1GRPH_UPDATE, 0);

			/* scaler update */
	xenos_write32(AVIVO_D1SCL_UPDATE, 1);
	xenos_write32(AVIVO_D1SCL_SCALER_ENABLE , 0);
	xenos_write32(0x2840, FB_BASE);
	xenos_write32(0x2844, mode->width);
	/* xboxkrnl (VdpInitializePrimarySurface) writes 0x2840 = 0 (surface base is set later),
	 * 0x2844 = pitch, and 0x2848 = 4; the meaning of the difference to 0x80000 is unknown. */
	xenos_write32(0x2848, 0x80000);

	xenos_write32(AVIVO_D1MODE_VIEWPORT_START , 0); /* viewport */
	xenos_write32(AVIVO_D1MODE_VIEWPORT_SIZE, (mode->width << 16) | (mode->active_height * interlace_factor));
	xenos_write32(0x65e8, (mode->width >> 2) - 1);
	xenos_write32(AVIVO_D1MODE_DATA_FORMAT, mode->is_progressive ? 0 : 1);
	xenos_write32(0x6550, 0xff);
	xenos_write32(0x6524, 0x300);
	xenos_write32(0x65d0, 0x100);
	xenos_write32(D1GRPH_FLIP_CONTROL, 1);

	xenos_write32(AVIVO_D1SCL_SCALER_TAP_CONTROL, mode->scl_tap);
	xenos_write32(AVIVO_D1MODE_VIEWPORT_SIZE, (mode->width << 16) | (mode->height * interlace_factor));
	xenos_write32(0x65e8, (mode->width / 4) - 1);
	xenos_write32(AVIVO_D1MODE_DESKTOP_HEIGHT, mode->height * interlace_factor);
	xenos_write32(AVIVO_D1MODE_VIEWPORT_START, 0);
	xenos_write32(D1GRPH_X_START, 0);
	xenos_write32(D1GRPH_Y_START, 0);
	xenos_write32(D1GRPH_X_END, mode->width);
	xenos_write32(D1GRPH_Y_END, mode->height * interlace_factor);
	xenos_write32(D1CRTC_MVP_FIFO_STATUS, 0);
	xenos_write32(D1CRTC_MVP_SLAVE_STATUS, 0);
	xenos_write32(D1CRTC_MVP_INBAND_CNTL_CAP, 0);
	xenos_write32(D1CRTC_MVP_INBAND_CNTL_INSERT, 0);
	xenos_write32(0x65a0, 0);
	xenos_write32(0x65b4, 0x01000000);
	xenos_write32(0x65c4, 0x01000000);
	xenos_write32(0x65b0, 0);
	xenos_write32(0x65c0, 0x01000000);
	xenos_write32(0x65b8, mode->scl_65b8);
	xenos_write32(0x65c8, mode->scl_65c8);
	xenos_write32(0x65dc, 0);
	xenos_write32(AVIVO_D1SCL_UPDATE, 0);

	xenos_write32(DC_LUTA_CONTROL, 0);
	xenos_write32(DC_LUT_RW_INDEX, 0);
	xenos_write32(DC_LUT_RW_MODE, 0);
	xenos_write32(DC_LUT_WRITE_EN_MASK, 7);
	xenos_write32(DC_LUT_AUTOFILL, 1);
	while (!(xenos_read32(DC_LUT_AUTOFILL) & 2));
}


/* ---- mode construction from the kernel tables ------------------------------------------------- */
static int xenos_region(const char *name)
{
	int r = xenon_config_get_avregion();
	if (!strcmp(name, "PAL50")) return AVREGION_PAL50;
	if (!strcmp(name, "PAL60")) return AVREGION_PAL60;
	if (r == AVREGION_NTSCM || r == AVREGION_NTSCJ) return r;
	if (r == AVREGION_PAL50 || r == AVREGION_PAL60) return AVREGION_PAL50;
	return AVREGION_NTSCM;
}

static int xenos_build_mode(int id, struct mode_s *m)
{
	int w = xenos_mode_defs[id].w, h = xenos_mode_defs[id].h, il = xenos_mode_defs[id].il;

	memset(m, 0, sizeof(*m));
	m->name = xenos_mode_defs[id].name;
	m->kind = xenos_mode_defs[id].kind;
	m->hz = xenos_mode_defs[id].hz;
	m->real_active_width = w;
	m->width = (w + 31) & ~31;
	m->is_progressive = !il;
	m->height = m->active_height = il ? h / 2 : h;
	m->ana_idx = -1;
	m->hdmi = m->kind == XM_HDMI;
	if (m->kind == XM_TV) {
		int e = ana_pick_tv(xenos_region(m->name), xenon_smc_read_avpack(), w, h, il, m->hz);
		if (e < 0)	/* not offered for this AV pack (e.g. HD on a composite pack) */
			return -1;
		m->ana_idx = e;
		ana_tv_geo(e, &m->total_width, &m->total_height, &m->hsync_offset, &m->vsync_offset);
		m->composite_sync = 1;
		m->rgb = 0;
		m->overscan = 1;
	} else {
		int t = ana_pick_timing(w, h);
		if (t < 0)
			return -1;
		m->ana_idx = t;
		ana_timing_crtc(t, m->hdmi, &m->total_width, &m->total_height, &m->hsync_offset, &m->vsync_offset);
		m->rgb = 1;
		m->overscan = m->hdmi;
	}
	/* display scaler (1:1) constants by size, kernel VdInitializeScaler */
	if (h >= 1080)      { m->scl_tap = 0x502; m->scl_65b8 = 0x40000; m->scl_65c8 = 0x28000; }
	else if (h >= 720)  { m->scl_tap = 0x904; m->scl_65b8 = 0x60000; m->scl_65c8 = 0x38000; }
	else                { m->scl_tap = 0x905; m->scl_65b8 = 0x60000; m->scl_65c8 = 0x40000; }
	return 0;
}

/* kernel VdpInitializeStaticTiming / VdpInitializeDynamicTiming + colour-space init */
static int xenos_ana_apply(struct mode_s *m)
{
	int ver = xenos_ana_version(), bl = xenos_black_level;

	if (m->kind == XM_TV) {
		ana_gen_tv(&xenos_ana_ops, m->ana_idx, ver, bl);
		m->sync_cntl = ana_dve_sync_tv(m->ana_idx);
	} else {
		uint8_t vt[76], rnd[8];
		int i, dtype = m->hdmi ? 3 : 1, port = m->hdmi ? ANA_PORT_HDMI : ANA_PORT_ANALOG;
		union { uint32_t u; float f; } c;
		memcpy(vt, ana_timing_blob(m->ana_idx), 76);
		if ((ver >= ANA_VER_KSB ? ana_calc_ksb_clocks(vt, dtype, port) : ana_calc_video_clocks(vt, dtype)) < 0)
			return -1;
		c.u = vt[20] << 24 | vt[21] << 16 | vt[22] << 8 | vt[23];
		m->pix_khz = (int)(c.f * 1000.0f + 0.5f);
		for (i = 0; i < 8; i++)
			rnd[i] = mftb() >> (i * 3);	/* HDCP An seed */
		ana_gen_dyn(&xenos_ana_ops, vt, ver, port, dtype, bl, rnd);
		m->sync_cntl = ana_dve_sync_dyn(vt, port);
		ana_gen_csc_vga(&xenos_ana_ops, bl);
	}
	return 0;
}

/* kernel HalGetCurrentAVPack: the SMC pack type, or 4 (8 for type 7) when a sink raises HPD on an HDMI-capable console */
static int xenos_kernel_avpack(int raw)
{
	int type = SMC_AVPACK_TYPE(raw);
	return (xenos_ana_version() >= ANA_VER_HDMI && SMC_AVPACK_HPD(raw)) ? (type == 7 ? 8 : 4) : type;
}

static uint32_t xenos_dve_cntl(struct mode_s *m)
{
	return m->kind == XM_TV ? ana_dve_cntl_tv(m->ana_idx) : m->hdmi ? ANA_DVE_CNTL_HDMI : ANA_DVE_CNTL_VGA;
}

/* kernel HalClampUnclampOutputDACs: clamp masks the analog outputs (and powers the HDMI lanes down), unclamp restores them;
 * also writes ANA_SPDIF_EN. Applies to the active mode. */
void xenos_clamp_dacs(int clamp)
{
	int raw = xenon_smc_read_avpack(), avpack = xenos_kernel_avpack(raw);
	struct ana_dac_cfg c;
	if (!xenos_current_mode)
		return;
	c.ver = xenos_ana_version();
	c.port = xenos_current_mode->hdmi;
	c.avpack = avpack;
	c.submode = SMC_AVPACK_SUBMODE(raw);
	c.hpd = c.ver >= ANA_VER_HDMI && SMC_AVPACK_HPD(raw);
	c.analog_unclamp = c.digital_en = !clamp;
	c.trinity_plus = xenon_get_console_type() >= REV_TRINITY;
	c.cntl = xenos_dve_cntl(xenos_current_mode);
	c.sync = xenos_current_mode->sync_cntl;
	ana_dac_request(&xenos_ana_ops, &c);
}

/* kernel VdDriverEntry: S/PDIF output follows the pack / console (HDMI-capable DVE only) */
static void xenos_spdif_init(void)
{
	int raw = xenon_smc_read_avpack(), avpack = xenos_kernel_avpack(raw);
	if (xenos_ana_version() < ANA_VER_HDMI)
		return;
	xenon_smc_ana_write(ANA_SPDIF_EN, ana_spdif_enabled(xenon_get_console_type() >= REV_TRINITY, avpack, !(avpack == 7 || avpack == 8), SMC_AVPACK_SUBMODE(raw)) ? ANA_SPDIF_ON : ANA_SPDIF_OFF);
}

static void xenos_set_gpio_video(struct mode_s *m)
{
	int reg = m->kind == XM_VGA ? 5 : 4 + !m->is_progressive;
	xenon_gpio_control(reg, XENOS_GPIO_VIDEO_MASK, XENOS_GPIO_VIDEO_MASK);
}

static int xenos_frame_us(struct mode_s *m)
{
	return 1000000 / m->hz;
}

int xenos_set_mode(struct mode_s *mode)
{
	int hdmi_on = mode->hdmi && xenos_hdmi_sink;

	if (hdmi_on) {
		hdmi_av_mute(&xenos_hdmi_ops, 1);
		udelay(xenos_frame_us(mode));
	}
	xenos_write32(GPU_ANA_CTRL, xenos_read32(GPU_ANA_CTRL) | 1);
	xenos_write32(GPU_MODE_LOCK, 1);	/* kernel VdSetDisplayMode: held while timing/CSC are reprogrammed */
	xenos_write32(0x06ac, 1);

	printf(" . ana disable\n");
	xenos_ana_preinit();
	xenos_write32(GPU_ANA_PWRDOWN_HOLD, 0x100);
	printf(" . ana enable\n");
	if (xenos_ana_apply(mode)) {
		printf("xenos: no ANA configuration for %s\n", mode->name);
		return -1;
	}
	xenos_write32(GPU_ANA_ENABLE, 1);

	printf(" . f1\n");
	xenos_set_mode_f1(mode);
	printf(" . f2\n");
	xenos_set_mode_f2(mode);

	xenos_write32(GPU_CRTC_CONTROL, 0x10001);

	if (!mode->composite_sync)
	{
		xenos_write32(D1CRTC_MVP_CONTROL1, 0x04010040);
		xenos_write32(D1CRTC_MVP_CONTROL2, 0);
		xenos_write32(D1CRTC_MVP_FIFO_CONTROL, 0x04010040);
	} else
	{
		xenos_write32(D1CRTC_MVP_CONTROL1, 0x20000200);
		xenos_write32(D1CRTC_MVP_CONTROL2, 0x20010200);
		xenos_write32(D1CRTC_MVP_FIFO_CONTROL, 0x20010200);
	}
	/* kernel VdpInitializeForceDataColor: RGB forces data colour 7 / 0, YCbCr 5 / 0x200 (bits 8-10 of the ANA control) */
	xenos_write32(GPU_FORCE_DATA_COLOR, mode->rgb ? 0 : 0x200);
	xenos_write32(GPU_ANA_CTRL, (xenos_read32(GPU_ANA_CTRL) & ~0x701) | (mode->rgb ? 0x700 : 0x500));


	xenos_write32(AVIVO_D1CRTC_V_BLANK_START_END, 0x04010040);
	xenos_write32(D1CRTC_MVP_INBAND_CNTL_INSERT_TIMER, 0x00010002);
	xenos_write32(D1CRTC_MVP_BLACK_KEYER, 0xec02414a);

	xenos_write32(D1CRTC_TRIGA_CNTL, 0x000000ec);
	xenos_write32(D1CRTC_TRIGA_MANUAL_TRIG, 0x014a00ec);
	xenos_write32(D1CRTC_TRIGB_CNTL, 0x00d4014a);
	xenos_write32(D1GRPH_PRIMARY_SURFACE_ADDRESS, FB_BASE);

	{	/* output colour matrix (kernel VdGenerateGPUCSCCoefficients): RGB range by black level, YCbCr BT.601 SD / BT.709 HD */
		const uint32_t *m = mode->rgb ? (xenos_black_level == 1 ? gpu_csc_rgb_bl1 : xenos_black_level == 2 ? gpu_csc_rgb_bl2 : gpu_csc_rgb_bl0)
					      : (mode->active_height * (mode->is_progressive ? 1 : 2) >= 720 ? gpu_csc_hd709 : gpu_csc_sd601);
		int i;
		xenos_write32(D1GRPH_COLOR_MATRIX_TRANSFORMATION_CNTL, 0x00000001);
		for (i = 0; i < 12; i++)
			xenos_write32(D1COLOR_MATRIX_COEF_1_1 + 4 * i, m[i]);
		xenos_write32(0x63b4, 0x00000000);
	}

	xenos_set_gpio_video(mode);
	if (mode->kind == XM_TV)	/* TV black level / range regs (kernel does this after the timing) */
		ana_gen_csc_tv(&xenos_ana_ops, mode->ana_idx, ana_avpack(xenon_smc_read_avpack()), xenos_black_level, xenos_ana_version());

	xenos_write32(GPU_MODE_LOCK, 0);
	{	/* kernel VdpTurnDisplayOnInternal: composite pack without HPD and sub mode bit0 drops DAC enables 1-2 */
		int raw = xenon_smc_read_avpack(), avpack = xenos_kernel_avpack(raw);
		uint32_t cntl = xenos_dve_cntl(mode);
		xenos_current_mode = mode;
		ana_display_on(&xenos_ana_ops, cntl, xenos_ana_version(), mode->hdmi, avpack, raw & 3);
	}
	if (hdmi_on) {
		hdmi_send_avi_rgb(&xenos_hdmi_ops, hdmi_cea_vic(mode->real_active_width, mode->active_height * (mode->is_progressive ? 1 : 2), !mode->is_progressive, mode->hz), mode->real_active_width * 9 == mode->active_height * 16, 1);
		hdmi_send_spd(&xenos_hdmi_ops);
		hdmi_av_mute(&xenos_hdmi_ops, 1);
		udelay(4 * xenos_frame_us(mode));
		hdmi_av_mute(&xenos_hdmi_ops, 0);
	}
	return 0;
}

static int xenos_mode_by_size(int kind, int w, int h)
{
	int i;
	for (i = 0; i < VIDEO_MODE_COUNT; i++)
		if (xenos_mode_defs[i].kind == kind && xenos_mode_defs[i].w == w && xenos_mode_defs[i].h == h && !xenos_mode_defs[i].il && xenos_mode_defs[i].hz == 60)
			return i;
	return -1;
}

/* digital sink: 720p unless the sink cannot do it, then 1080p */
static int xenos_pick_hdmi(struct edid *e)
{
	if (!e || xenos_edid_cea_vic(e, 4) || xenos_edid_supports(e, 1280, 720))
		return VIDEO_MODE_HDMI_720P;
	if (xenos_edid_cea_vic(e, 16) || xenos_edid_supports(e, 1920, 1080))
		return VIDEO_MODE_HDMI_1080P;
	return VIDEO_MODE_HDMI_720P;
}

/* analog VGA: native timing if we have it, else the largest common size the sink lists */
static int xenos_pick_vga(struct edid *e)
{
	int w, h, m;
	if (xenos_edid_native(e, &w, &h) && (m = xenos_mode_by_size(XM_VGA, w, h)) >= 0)
		return m;
	if (xenos_edid_supports(e, 1024, 768) || !e)
		return VIDEO_MODE_VGA_1024x768;
	return VIDEO_MODE_VGA_640x480;
}

/* AV pack (SMC byte, kernel HalpReadCurrentAVPack/VdpEDIDDiscovery): type = bits 4:2, bit6 = no hot-plug.
 * HDMI-capable consoles (DVE >= 0x11) report a sink via HPD whatever the pack type; type 6 is VGA; type 3 is
 * component (HD); everything else (composite, S-video, SCART, HDMI pack without a sink) is the region's TV mode.
 * Raw bytes seen in the field: 0x43/0x47/0x54/0x57 TV, 0x0c/0x0f/0x4f component, 0x59/0x5b VGA,
 * 0x13/0x14/0x1c/0x1e/0x1f HDMI with sink, 0x1b VGA adapter that raises HPD. */
static int xenos_pick_auto_mode(void)
{
	int mode;
	int avpack = xenon_smc_read_avpack();
	int type = SMC_AVPACK_TYPE(avpack);
	int hpd = xenos_ana_version() >= ANA_VER_HDMI && SMC_AVPACK_HPD(avpack);

	switch (xenon_config_get_avregion())
	{
	case AVREGION_NTSCJ:
	case AVREGION_NTSCM:
		mode = VIDEO_MODE_NTSC;
		break;
	case AVREGION_PAL50:
		mode = VIDEO_MODE_PAL50;
		break;
	case AVREGION_PAL60:
	default:
		mode = VIDEO_MODE_PAL60;
		break;
	}

	printf("AVPACK detected: %02x\n", avpack);

	if (hpd || type == 6) {
		int digital;
		xenos_edid = xenos_get_edid();
		digital = xenos_edid && (xenos_edid->input & DRM_EDID_INPUT_DIGITAL);
		/* digital sink, or a sink-less HDMI pack: HDMI port; analog EDID / VGA pack: VGA */
		mode = (digital || (!xenos_edid && hpd && type != 6)) ? xenos_pick_hdmi(xenos_edid) : xenos_pick_vga(xenos_edid);
	} else if (type == 3)
		mode = VIDEO_MODE_YUV_720P;

	return mode;
}

void xenos_init(int videoMode)
{
	xenos_id = xenon_get_XenosID();
	xenos_asic = (xenos_id << 16) | (read32(0xd0010008) & 0xff);
	printf("Xenos GPU ID=%04x\n", (unsigned int)xenos_id);

	xenos_is_corona = isCoronaOrWinchester();
	if (xenos_is_corona)
		printf("Detected Corona motherboard!\n");

	xenos_init_phase0();
	xenos_init_phase1();

	xenon_gpio_set(0, 0x2300);
	xenon_gpio_set_oe(0, 0x2300);

	if (videoMode <= VIDEO_MODE_AUTO || videoMode >= VIDEO_MODE_COUNT){
		xenon_config_init();
		videoMode = xenos_pick_auto_mode();
	}

	if (xenos_build_mode(videoMode, &xenos_mode_cur)) {
		printf("xenos: mode %d not available\n", videoMode);
		return;
	}
	if (xenos_mode_cur.hdmi) {
		if (!xenos_edid)
			xenos_edid = xenos_get_edid();
		xenos_hdmi_sink = xenos_detect_hdmi_monitor(xenos_edid);
		if (xenos_hdmi_sink)
			printf("Detected HDMI monitor!\n");
	}
	if (xenos_set_mode(&xenos_mode_cur))
		return;
	xenos_cur_id = videoMode;

	xenos_spdif_init();

	xenos_hdmi_audio_enable();

	printf("Video Mode: %s\n", xenos_current_mode->name);
}

/* ---- switching the video mode of a running application ---- */
int xenos_video_mode_count(void)
{
	return VIDEO_MODE_COUNT;
}

const char *xenos_video_mode_name(int id)
{
	return id >= 0 && id < VIDEO_MODE_COUNT ? xenos_mode_defs[id].name : NULL;
}

/* framebuffer size of a mode; -1 if the mode is not offered for this AV pack / region */
int xenos_video_mode_size(int id, int *width, int *height)
{
	struct mode_s m;

	if (id < 0 || id >= VIDEO_MODE_COUNT || xenos_build_mode(id, &m))
		return -1;
	*width = m.width;
	*height = m.height * (m.is_progressive ? 1 : 2);
	return 0;
}

int xenos_video_mode_current(void)
{
	return xenos_cur_id;
}

/* build and apply mode id; the caller resizes its framebuffer afterwards. 0 on success */
int xenos_video_mode_set(int id)
{
	struct mode_s m;

	if (id < 0 || id >= VIDEO_MODE_COUNT || xenos_build_mode(id, &m))
		return -1;
	xenos_mode_cur = m;
	if (xenos_mode_cur.hdmi) {
		if (!xenos_edid)
			xenos_edid = xenos_get_edid();
		xenos_hdmi_sink = xenos_detect_hdmi_monitor(xenos_edid);
	}
	if (xenos_set_mode(&xenos_mode_cur))
		return -1;
	xenos_cur_id = id;
	xenos_hdmi_audio_enable();
	return 0;
}

int xenos_is_overscan()
{
	if(xenos_current_mode)
		return xenos_current_mode->overscan;
	else{
		printf("Xenos not initialized\n");
		exit(-1);
	}
}

int xenos_is_initialized()
{
	return xenos_current_mode != NULL;
}
