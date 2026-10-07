#ifndef __xenos_hdmi_regs_h
#define __xenos_hdmi_regs_h
/* Hana HDMI transmitter, SMC I2C space (xenon_smc_i2c_*). Names from xboxkrnl Vdp* usage. */
#define HDMI_TX_AUDIO_MODE     0x214  /* 0x11 stereo LPCM; 0x03/0x07 multichannel (kernel: pixclk dependent) */
#define HDMI_TX_AUDIO_CFG      0x202  /* 0x03 */
#define HDMI_TX_AUDIO_N0       0x203  /* N[7:0]   audio clock regeneration N (20 bit) */
#define HDMI_TX_AUDIO_N1       0x204  /* N[15:8] */
#define HDMI_TX_AUDIO_N2       0x205  /* N[19:16] */
#define HDMI_TX_AUDIO_0x21D    0x21d  /* 0x40 (unnamed) */
#define HDMI_TX_AUDIO_CS_FS    0x221  /* IEC60958 channel status: 2 = 48 kHz [I] */
#define HDMI_TX_AUDIO_CS_ORIG  0x222  /* 0x2b [I] */
#define HDMI_TX_HDMI_MODE      0x22f  /* bit0 = HDMI (not DVI) signalling */
#define HDMI_TX_PKT_ENABLE     0x23e  /* infoframe/packet enable mask (kernel: read-modify-write, wait for ack) */
#define HDMI_TX_AVMUTE_LATCH   0x23f  /* bit3 = apply, self-clearing */
#define HDMI_TX_AVMUTE         0x2df  /* 0x10 = unmuted, 0x01 = AV mute */
/* packet RAM base / enable-bit index (PKT_ENABLE bits idx and idx-1) */
#define HDMI_TX_PKT_AVI       0x240
#define HDMI_TX_PKT_AVI_IDX   1
#define HDMI_TX_PKT_SPD       0x260
#define HDMI_TX_PKT_SPD_IDX   3
#define HDMI_TX_PKT_AUDIO     0x280
#define HDMI_TX_PKT_AUDIO_IDX 5
#define HDMI_TX_AVMUTE_PENDING 0x08   /* HDMI_TX_AVMUTE_LATCH bit3 */
#define HDMI_AVMUTE_ON        0x01
#define HDMI_AVMUTE_OFF       0x10

#define HDMI_AUDIO_N_6144      6144   /* 25.2, 27, 74.25, 148.5 MHz TMDS */
#define HDMI_AUDIO_N_11648     11648  /* 74.176 MHz (720p/1080 59.94) */
#define HDMI_AUDIO_N_5824      5824   /* 148.352 MHz */
#define HDMI_AUDIO_N_6864      6864   /* 25.175 MHz */

/* SMC AV pack byte (cmd 0x0f): [1:0] sub mode, [4:2] pack, bit6 = no HPD (kernel HalpHPDDetected = !bit6) */
#define SMC_AVPACK_SUBMODE(b)  ((b) & 3)
#define SMC_AVPACK_TYPE(b)     (((b) >> 2) & 7)
#define SMC_AVPACK_HPD(b)      (!((b) & 0x40))
/* SMC 0x8d: [1] bit0 = audio enable (kernel HalSetAudioEnable) */
#define SMC_CMD_AUDIO_ENABLE   0x8d
#endif
