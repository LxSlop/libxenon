/* Hana HDMI transmitter: infoframes, AV mute, audio clock — mirrors xboxkrnl Vdp{SetAVMute,Initialize*InfoFrame,GenerateInfoFramePacket} (RGB, LPCM stereo) */
#ifndef XENOS_HDMI_H
#define XENOS_HDMI_H
#include <stdint.h>
#include "xenos_hdmi_regs.h"

struct hdmi_ops {
	void    *ctx;
	void     (*wr)(void *ctx, uint16_t reg, uint8_t val);
	uint8_t  (*rd)(void *ctx, uint16_t reg);
	void     (*delay_us)(void *ctx, uint32_t us);
};

int  hdmi_audio_n(int pix_khz);                      /* audio clock regeneration N for the TMDS clock */
int  hdmi_cea_vic(int w, int h, int interlaced, int hz); /* 0 if none */
void hdmi_audio_setup(const struct hdmi_ops *o, int pix_khz);  /* LPCM 48 kHz stereo: 0x214.. + HDMI mode + packet enable */
void hdmi_send_avi_rgb(const struct hdmi_ops *o, int vic, int widescreen, int pixel_rep);
void hdmi_send_spd(const struct hdmi_ops *o);
void hdmi_send_audio_if(const struct hdmi_ops *o);   /* stereo LPCM */
void hdmi_av_mute(const struct hdmi_ops *o, int mute);
#endif
