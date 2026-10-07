#include "xenos_hdmi.h"

#define WR(r, v)  o->wr(o->ctx, (r), (v))
#define RD(r)     o->rd(o->ctx, (r))
#define POLL_MAX  500   /* kernel: 2 ms stalls up to 1 s */

int hdmi_audio_n(int k)
{
	if (k > 25100 && k < 25250 && k != 25200) return HDMI_AUDIO_N_6864;
	if (k > 74100 && k < 74200) return HDMI_AUDIO_N_11648;
	if (k > 148300 && k < 148400) return HDMI_AUDIO_N_5824;
	return HDMI_AUDIO_N_6144;
}

int hdmi_cea_vic(int w, int h, int il, int hz)
{
	if (w == 640 && h == 480 && !il) return 1;
	if (w == 720 && h == 480 && !il) return 3;
	if (w == 1280 && h == 720 && !il) return hz == 50 ? 19 : 4;
	if (w == 1920 && h == 1080) return il ? (hz == 50 ? 20 : 5) : (hz == 50 ? 31 : 16);
	if (w == 720 && h == 576 && !il) return 18;
	return 0;
}

static void pkt_write(const struct hdmi_ops *o, uint8_t *d, int len, int csum_idx, uint16_t base, int idx)
{
	unsigned mask = (1u << idx) | (1u << (idx - 1)), cur = RD(HDMI_TX_PKT_ENABLE);
	int i, n;
	uint8_t sum = 0;

	if (cur & (1u << idx)) {		/* active: disable, wait until the transmitter drops it */
		cur &= ~(1u << (idx - 1));
		WR(HDMI_TX_PKT_ENABLE, cur);
		for (n = 0; n < POLL_MAX && (RD(HDMI_TX_PKT_ENABLE) & (1u << idx)); n++)
			o->delay_us(o->ctx, 2000);
	}
	for (i = 0; i < len; i++) sum += d[i];
	d[csum_idx] = -sum;
	for (i = 0; i < len; i++) WR(base + i, d[i]);
	WR(HDMI_TX_PKT_ENABLE, cur | mask);
}

void hdmi_send_avi_rgb(const struct hdmi_ops *o, int vic, int widescreen, int rep)
{
	uint8_t d[17] = { 0x82, 2, 13, 0, 0x10, 0x08 | (widescreen ? 0x20 : 0x10), 0, (vic == 1 && widescreen) ? 0 : vic & 0x7f, rep > 1 ? rep - 1 : 0 };
	if (!(RD(HDMI_TX_HDMI_MODE) & 1)) return;
	pkt_write(o, d, 17, 3, HDMI_TX_PKT_AVI, HDMI_TX_PKT_AVI_IDX);
}

void hdmi_send_spd(const struct hdmi_ops *o)
{
	uint8_t d[29] = { 0x83, 1, 0x19, 0, 'M', 'S', 'F', 'T', 0, 0, 0, 0, 'X', 'b', 'o', 'x', ' ', '3', '6', '0' };
	d[28] = 8;	/* source device info */
	if (!(RD(HDMI_TX_HDMI_MODE) & 1)) return;
	pkt_write(o, d, 29, 3, HDMI_TX_PKT_SPD, HDMI_TX_PKT_SPD_IDX);
}

void hdmi_send_audio_if(const struct hdmi_ops *o)
{
	uint8_t d[14] = { 0x84, 1, 10, 0, 0, 0, 0x50, 0 };
	if (!(RD(HDMI_TX_HDMI_MODE) & 1)) return;
	pkt_write(o, d, 14, 3, HDMI_TX_PKT_AUDIO, HDMI_TX_PKT_AUDIO_IDX);
}

void hdmi_audio_setup(const struct hdmi_ops *o, int pix_khz)
{
	int n = hdmi_audio_n(pix_khz);
	WR(HDMI_TX_AUDIO_MODE, 0x11);
	WR(HDMI_TX_AUDIO_CFG, 0x03);
	WR(HDMI_TX_AUDIO_N0, n & 0xff);
	WR(HDMI_TX_AUDIO_N1, (n >> 8) & 0xff);
	WR(HDMI_TX_AUDIO_N2, (n >> 16) & 0xf);
	WR(HDMI_TX_AUDIO_0x21D, 0x40);
	WR(HDMI_TX_AUDIO_CS_FS, 0x02);
	WR(HDMI_TX_AUDIO_CS_ORIG, 0x2b);
	WR(HDMI_TX_HDMI_MODE, 0x01);
	hdmi_send_audio_if(o);
}

static void latch_wait(const struct hdmi_ops *o)
{
	int n;
	for (n = 0; n < POLL_MAX && (RD(HDMI_TX_AVMUTE_LATCH) & HDMI_TX_AVMUTE_PENDING); n++)
		o->delay_us(o->ctx, 2000);
}

void hdmi_av_mute(const struct hdmi_ops *o, int mute)
{
	unsigned v, base;
	int i;
	if (!(RD(HDMI_TX_HDMI_MODE) & 1)) return;
	v = RD(HDMI_TX_AVMUTE_LATCH);
	if (v & HDMI_TX_AVMUTE_PENDING) {
		v &= 0xfb;
		WR(HDMI_TX_AVMUTE_LATCH, v);
		latch_wait(o);
	}
	WR(HDMI_TX_AVMUTE, mute ? HDMI_AVMUTE_ON : HDMI_AVMUTE_OFF);
	base = v & 0xf3;
	for (i = 0; i < 2; i++) {
		WR(HDMI_TX_AVMUTE_LATCH, base | HDMI_TX_AVMUTE_PENDING);
		latch_wait(o);
	}
	if (!mute)
		WR(HDMI_TX_AVMUTE_LATCH, base | 0x0c);
}
