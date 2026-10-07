/* Hana/ANA (DVE) register names. Evidence: xboxkrnl function names + decoded formulas (see RESEARCH.md). */
#ifndef ANA_REGS_H
#define ANA_REGS_H

/* access: SMC cmd 0x11 (all regs), GPU 0x7950/0x7954 "XDVO" (<0xff, not Corona), SMC I2C (>=0x100, HDMI TX) */
#define GPU_ANA_ENABLE   0x7900
#define GPU_ANA_SEL      0x7910
#define GPU_ANA_XDVO_REG 0x7950
#define GPU_ANA_XDVO_VAL 0x7954
#define GPU_ANA_CTRL     0x7938   /* bit0 cleared around XDVO bursts */

#define ANA_DVE_CNTL      0x00
#define   ANA_CNTL_OUTPUTS   0x1e       /* TurnDisplayOff clears */
#define   ANA_CNTL_IDLE      0x60
#define   ANA_CNTL_VGA       0x4f
#define   ANA_CNTL_HDMI_RST  0x181      /* then 0x81 after 5 ms */
#define   ANA_CNTL_HDMI_PRE  0xc1
#define   ANA_CNTL_HDMI_ON   0x2c1
#define ANA_CC_STATUS     0x01
#define ANA_CC_CTRL       0x03
#define ANA_CC_DATA0      0x04
#define ANA_CC_DATA1      0x05
#define ANA_SUBCARRIER0   0x06          /* [I] */
#define ANA_SUBCARRIER1   0x07          /* [I] */
#define ANA_WSS_APS       0x08
#define ANA_MACROVISION0  0x09
#define ANA_MACROVISION1  0x0a
#define ANA_MACROVISION2  0x0b
#define ANA_MODE_CLK      0x0c          /* 0x86000000 | (class-1)<<27 | 0xc0 | progressive?0x10 */
#define ANA_PIXIN_CNTL    0x0d          /* GET_DVE_PIXINCNTL(=1) | 8 if RGB input; 9 VGA/HDMI, 1 TV */
#define ANA_OUT_RANGE     0x0e          /* CSC init: TV 0x1c0ffc40 / 0x3c0ffc40 (ver>=0x11), RGB 0x1ffffc00 */
#define ANA_BLACK_SETUP   0x0f          /* NTSC-M 0x064b0000/0x066c0000/0x065b0000 (bl0/1/2), PAL 0x06cf00d8, RGB 0x066c0000 */
#define ANA_CLK_THRESH    0x10          /* pixclk < 85 MHz ? 0x24900000 : 0 */
#define ANA_OUT_GAIN      0x11          /* black-level dependent: VGA 0x87400/0x9d800/0x91800 */
#define ANA_SYNC_CNTL     0x12          /* | 0x300 for table modes */
#define ANA_H_TIMING      0x13          /* [31:23] hsync+hbp  [22:12] hactive  [11:0] htotal   (all /pixel-rep) */
#define ANA_H_SYNC        0x14          /* [23:12] htotal-hsync+1  [11:0] hsync+1 */
#define ANA_V_TIMING      0x15
#define ANA_V_AUX         0x16
#define ANA_SRATE         0x17          /* ana_srate[pixclk class] */
#define ANA_WSS2_BASE     0x18          /* 0x18..0x1e, DVE >= 0x20 */
#define ANA_COMMON_BASE   0x20          /* 0x20..0x25 (+0x02) */
#define ANA_LVL_A         0x26          /* 15 */
#define ANA_LVL_B         0x35          /* 15 */
#define ANA_LVL_B2        0x44          /* 15 */
#define ANA_LVL_C         0x53          /* 2  */
#define ANA_LVL_D         0x55          /* 3  */
#define ANA_ZERO_BASE     0x6d          /* 0x6d..0x76 zero-init; 0x75/0x76 read back as status */
#define ANA_HDMI_EN       0x77
#define ANA_HDMI_K0       0x78
#define ANA_HDMI_K1       0x79
#define ANA_HDCP_AN_LO    0x7d
#define ANA_HDCP_AN_HI    0x7e
#define ANA_HDCP_STATUS   0x7f
#define ANA_KSB_CTRL      0xcd          /* 0x62; |0x10 pulse; &~0x40 */
#define ANA_KSB_CF        0xcf          /* 0x0854acc0; bit30 / bit27 sequence */
#define ANA_PLL_D0        0xd0          /* KSB: see ana_ksb_pll() */
#define ANA_PLL_D1        0xd1          /* KSB: den<<16 | num */
#define ANA_PLL_D2        0xd2          /* KSB: sign<<15 | |spread| (hi half = status) */
#define ANA_KSB_D3        0xd3          /* 0x1b0a659d -> 0x1b02659d */
#define ANA_PLL_PWR0      0xd5          /* Hana: 0x60 */
#define ANA_PLL_PWR1      0xd6          /* Hana: 0x60, +1 ms */
#define ANA_DAC_PWR       0xd7          /* 0xff / 0x7f (down), |0x80 */
#define ANA_DAC_EN        0xd8          /* 7 VGA, 0xb/0xf TV, 0x1f KSB */
#define ANA_POWER         0xd9          /* bit18: powered down */
#define ANA_SPDIF_EN      0xdf          /* 1 = S/PDIF output enabled (kernel VdDriverEntry / DAC request) */
#define   ANA_SPDIF_OFF   0
#define   ANA_SPDIF_ON    1
#define ANA_DF            ANA_SPDIF_EN
#define ANA_DAC_CAL0      0xf0          /* KSB 0x0291028e, bit31 pulse */
#define ANA_DAC_CAL1      0xf1          /* KSB 0x028e0285 */
#define ANA_HDCP_KEY0     0xfc
#define ANA_HDCP_KEY1     0xfd
#define ANA_VERSION       0xfe          /* ver = (r & 0xf) | ((r >> 12) & 0xf0) */
/* DVE version thresholds (compare with >=); observed: 0x01 old, 0x11 HDMI Hana, 0x23 Corona */
enum ana_version {
	ANA_VER_XENON = 0x01,
	ANA_VER_HDMI  = 0x11,	/* HDMI-capable Hana (Falcon/Jasper/Trinity) */
	ANA_VER_KSB   = 0x20,	/* Corona/Winchester: KSB PLL, new DAC init, WSS2 regs */
};

#define ANA_DELAY_1MS     0x8000        /* flag on ana_smb_addr[]: 1 ms delay after the write */
#define ANA_ZERO_END      0x76
#define ANA_COMMON_N      17            /* ana_common_addr[] */
#define ANA_I2C_TX_ID     0x102
#define ANA_I2C_PWR       0x108         /* 0x36 at power-down (DVE >= 0x11) */
#define ANA_I2C_LANE_PWR0 0x1a2         /* 1a2/1a6/1aa/1ae: 0x10 = off, 0 = on (DVE>=0x20 HDMI) */
#define ANA_I2C_DDC_CMD   0x1ec
#define ANA_I2C_INFOFRAME 0x22f

/* Hana HDMI TX I2C regs written by the dynamic timing path (names [I] from the values) */
#define ANA_I2C_CFG_LOCK  0x105         /* 1 while programming, 0 after */
#define ANA_I2C_ENABLE    0x108         /* 0x37 enable, 0x36 power-down */
#define ANA_I2C_HSTART    0x132         /* 0x132/0x133: (hsync+hbp)/rep | flags 0x4000/0x2000/0x1000 */
#define ANA_I2C_VSTART    0x134         /* vsync + vbp */
#define ANA_I2C_HACTIVE   0x136         /* 0x136/0x137: hactive/rep */
#define ANA_I2C_VID_ID    0x138         /* VD_VIDEO_TIMING byte 11 */
#define ANA_I2C_VID_ID2   0x139
#define ANA_I2C_INTERLACE 0x13e         /* 0 when interlaced */
#define ANA_I2C_0x148     0x148         /* 0x10 */
#define ANA_I2C_CLK_RANGE 0x180         /* pixclk >= 100 MHz ? 27 : 9 */
#define ANA_I2C_PIXREP    0x182         /* 0x20 x1, 0x40 x2, 0x60 x4 */
#define ANA_I2C_LANE_CFG0 0x194         /* 0x33 */
#define ANA_I2C_LANE_CFG1 0x195         /* 0x33 */
#define ANA_I2C_LANE_CFG2 0x196         /* 0x11 */
#define ANA_I2C_LANE_CFG3 0x197         /* 0x11 */
#define ANA_I2C_LANE_PWR1 0x1a6
#define ANA_I2C_LANE_PWR2 0x1aa
#define ANA_I2C_LANE_PWR3 0x1ae
#define ANA_I2C_AVMUTE    0x2df         /* see xenos_hdmi_regs.h HDMI_TX_AVMUTE */

#endif
