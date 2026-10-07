#include "xenon_smc.h"
#include "../xenon_uart/xenon_uart.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pci/io.h>
#include <stdarg.h> //For custom printf

#define SMC_BASE 0xea001000
#define SMC_FIFO_TX      0x80	/* 4 x BE32 = one 16 byte message */
#define SMC_STATUS_TX    0x84
#define SMC_FIFO_RX      0x90
#define SMC_STATUS_RX    0x94
#define SMC_STATUS_READY 4	/* TX ready / RX pending; write 4 to acknowledge */

#define SMC_MSG_BULK         0x83	/* asynchronous notification */
#define SMC_BULK_POWER_BTN   0x11
#define SMC_BULK_POWER_REMOTE 0x20
#define SMC_BULK_IR          0x23
#define SMC_BULK_TRAY_FIRST  0x60
#define SMC_BULK_TRAY_LAST   0x65

#define SMC_CMD_SENSORS      0x07
#define SMC_CMD_AVPACK       0x0f
#define SMC_CMD_BUS          0x11	/* SMBus/I2C proxy */
#define SMC_CMD_POWER        0x82
#define SMC_CMD_FAN_ALGO     0x88
#define SMC_CMD_POWER_LED    0x8c
#define SMC_CMD_LEDS         0x99
#define SMC_POWER_OFF        0x01
#define SMC_POWER_REBOOT     0x04
#define SMC_REASON_RESET     0x30

/* SMC_CMD_BUS: [1] hi nibble = operation, lo nibble bit0 = DDC bus variant */
#define SMC_BUS_READ         0x10
#define SMC_BUS_I2C_WRITE    0x20
#define SMC_BUS_ANA_WRITE    0x60
#define SMC_BUS_DDC          0x01
#define SMC_BUS_DDC_LOCK     3
#define SMC_BUS_DDC_UNLOCK   5
#define SMC_BUS_SEL_I2C      1	/* [2] on reads */
#define SMC_BUS_SEL_ANA      5
#define SMC_DEV_VALID        0x80	/* [3]/[5] = 0x80 | 7-bit address */
#define SMC_DEV_ANA          0x70
#define SMC_DEV_HDMI_TX      0x39	/* I2C regs < 0x200 */
#define SMC_DEV_HDMI_TX_HI   0x3d	/* I2C regs >= 0x200 */
#define SMC_I2C_HI_FIRST     0x200
#define SMC_I2C_DDC_FIRST    0x1d0
#define SMC_I2C_DDC_LAST     0x1f5

void uprintf(const char* format, ...) {
	va_list args;
    va_start(args, format);
    char tmp[2048];
	vsprintf(tmp, format, args);
	uart_puts(tmp);
    va_end(args);
}

void xenon_smc_send_message(const unsigned char *msg)
{
/*	uprintf("SEND: ");
	int i;
	for (i = 0; i < 16; ++i)
		uprintf("%02x ", msg[i]);
	uprintf("\n");
*/
	while (!(read32(SMC_BASE + SMC_STATUS_TX) & SMC_STATUS_READY));
	write32(SMC_BASE + SMC_STATUS_TX, SMC_STATUS_READY);
	write32n(SMC_BASE + SMC_FIFO_TX, *(uint32_t*)(msg + 0));
	write32n(SMC_BASE + SMC_FIFO_TX, *(uint32_t*)(msg + 4));
	write32n(SMC_BASE + SMC_FIFO_TX, *(uint32_t*)(msg + 8));
	write32n(SMC_BASE + SMC_FIFO_TX, *(uint32_t*)(msg + 12));
	write32(SMC_BASE + SMC_STATUS_TX, 0);
}

int xenon_smc_receive_message(unsigned char *msg)
{
	if (read32(SMC_BASE + SMC_STATUS_RX) & SMC_STATUS_READY)
	{
		uint32_t *msgl = (uint32_t*)msg;
		int i;
		write32(SMC_BASE + SMC_STATUS_RX, SMC_STATUS_READY);
		for (i = 0; i < 4; ++i)
			*msgl++ = read32n(SMC_BASE + SMC_FIFO_RX);
		write32(SMC_BASE + SMC_STATUS_RX, 0);
		return 0;
	}
	return -1;
}

/* store the last IR keypress */
int xenon_smc_last_ir = -1;

int xenon_smc_get_ir() {
	int ret = xenon_smc_last_ir;
	xenon_smc_last_ir = -1;
	return ret;
}

void xenon_smc_handle_bulk(unsigned char *msg)
{
	switch (msg[1])
	{
	case SMC_BULK_POWER_BTN:
	case SMC_BULK_POWER_REMOTE:
		uprintf("SMC power message\n");
		break;
	case SMC_BULK_IR:
		//uprintf("IR RX [%02x %02x]\n", msg[2], msg[3]);
		xenon_smc_last_ir = msg[3];
		break;
	case SMC_BULK_TRAY_FIRST ... SMC_BULK_TRAY_LAST:
		
		uprintf("DVD cover state: %02x\n", msg[1]);
		break;
	default:
		uprintf("unknown SMC bulk msg: %02x\n", msg[1]);
		break;
	}
}

int xenon_smc_poll()
{
	uint8_t buf[16];
	memset(buf, 0, 16);

	if (!xenon_smc_receive_message(buf)) {
		if (buf[0] == SMC_MSG_BULK)
		{
			xenon_smc_handle_bulk(buf);
		}
		return 0;
	}
	return -1;
}

int xenon_smc_receive_response(unsigned char *msg)
{
	while (1)
	{
		if (xenon_smc_receive_message(msg))
			continue;

/*		uprintf("REC: ");
		int i;
		for (i = 0; i < 16; ++i)
			uprintf("%02x ", msg[i]);
		uprintf("\n");
*/
		if (msg[0] == SMC_MSG_BULK)
		{
			xenon_smc_handle_bulk(msg);
			continue;
		}
		return 0;
	}
}

int xenon_smc_ana_write(uint8_t addr, uint32_t val)
{
	uint8_t buf[16];
	
	memset(buf, 0, 16);
	
	buf[0] = SMC_CMD_BUS;
	buf[1] = SMC_BUS_ANA_WRITE;
	buf[3] = SMC_DEV_VALID | SMC_DEV_ANA;
	
	buf[6] = addr;
	buf[7] = 4;	/* byte count (kernel sets it) */
	
	buf[8] = val;
	buf[9] = val >> 8;
	buf[10] = val >> 16;
	buf[11] = val >> 24;

	xenon_smc_send_message(buf);

	xenon_smc_receive_response(buf);
	if (buf[1] != 0)
	{
		uprintf("xenon_smc_ana_write failed, addr=%02x, err=%d\n", addr, buf[1]);
		return -1;
	}
	
	return 0;
}

int xenon_smc_ana_read(uint8_t addr, uint32_t *val)
{
	uint8_t buf[16];
	memset(buf, 0, 16);

	buf[0] = SMC_CMD_BUS;
	buf[1] = SMC_BUS_READ;
	buf[2] = SMC_BUS_SEL_ANA;
	buf[3] = buf[5] = SMC_DEV_VALID | SMC_DEV_ANA;
	buf[6] = addr;
	
	xenon_smc_send_message(buf);
	xenon_smc_receive_response(buf);
	if (buf[1] != 0)
	{
		uprintf("xenon_smc_ana_read failed, addr=%02x, err=%d\n", addr, buf[1]);
		return -1;
	}
	*val = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);
	return 0;
}

int xenon_smc_i2c_ddc_lock(int lock)
{
	uint8_t buf[16];
    memset(buf, 0, 16);
	
	buf[0] = SMC_CMD_BUS;
	buf[1] = (lock) ? SMC_BUS_DDC_LOCK : SMC_BUS_DDC_UNLOCK;

	xenon_smc_send_message(buf);

	xenon_smc_receive_response(buf);
	if (buf[1] != 0)
	{
		uprintf("xenon_smc_i2c_ddc_lock failed, err=%d\n", buf[1]);
		return -1;
	}
	
	return 0;
}

int xenon_smc_i2c_write(uint16_t addr, uint8_t val)
{
	uint8_t buf[16];
    memset(buf, 0, 16);
	
	int tmp=(addr>=SMC_I2C_HI_FIRST)?SMC_DEV_HDMI_TX_HI:SMC_DEV_HDMI_TX;
	int ddc=(addr>=SMC_I2C_DDC_FIRST && addr<=SMC_I2C_DDC_LAST);

    buf[0] = SMC_CMD_BUS;
	buf[1] = SMC_BUS_I2C_WRITE | (ddc ? SMC_BUS_DDC : 0);
	buf[3] = tmp | SMC_DEV_VALID;
	
	buf[6] = addr & 0xff; //3a
	buf[7] = val;

	xenon_smc_send_message(buf);

	xenon_smc_receive_response(buf);
	if (buf[1] != 0)
	{
		uprintf("xenon_smc_i2c_write failed, addr=%04x, err=%d\n", addr, buf[1]);
		return -1;
	}
	
	return 0;
}

int xenon_smc_i2c_read(uint16_t addr, uint8_t *val)
{
	uint8_t buf[16];
	memset(buf, 0, 16);

    int tmp=(addr>=SMC_I2C_HI_FIRST)?SMC_DEV_HDMI_TX_HI:SMC_DEV_HDMI_TX;
	int ddc=(addr>=SMC_I2C_DDC_FIRST && addr<=SMC_I2C_DDC_LAST);

	buf[0] = SMC_CMD_BUS;
	buf[1] = SMC_BUS_READ | (ddc ? SMC_BUS_DDC : 0);
	buf[2] = SMC_BUS_SEL_I2C;
	buf[3] = buf[5] = tmp | SMC_DEV_VALID;
    buf[6] = addr & 0xff; //3a
	
	xenon_smc_send_message(buf);
	xenon_smc_receive_response(buf);
	if (buf[1] != 0)
	{
		uprintf("xenon_smc_i2c_read failed, addr=%04x, err=%d\n", addr, buf[1]);
		return -1;
	}
	*val = buf[3];
	return 0;
}

void xenon_smc_set_led(int override, int value)
{
	uint8_t buf[16];
	memset(buf, 0, 16);
	
	buf[0] = SMC_CMD_LEDS;
	buf[1] = override;
	buf[2] = value;
	
	xenon_smc_send_message(buf);
}

void xenon_smc_set_power_led(int override, int state, int startanim)
{
	uint8_t buf[16];
	memset(buf, 0, 16);

	buf[0] = SMC_CMD_POWER_LED;
	buf[1] = (override ? 1 : 0) | (state ? 0 : 2);
	buf[2] = startanim;

	xenon_smc_send_message(buf);
}

void xenon_smc_power_shutdown(void)
{
	uint8_t buf[16] = {SMC_CMD_POWER, SMC_POWER_OFF};
	xenon_smc_send_message(buf);
}

void xenon_smc_power_reboot(void)
{
	uint8_t buf[16] = {SMC_CMD_POWER, SMC_POWER_REBOOT, SMC_REASON_RESET, 0x00};
	xenon_smc_send_message(buf);
}

void xenon_smc_start_bootanim(void)
{
	uint8_t buf[16] = {SMC_CMD_POWER_LED, 0x01, 0x00,0,0,0,0,0,0,0,0,0,0,0,0,0};
	xenon_smc_send_message(buf);
	buf[2] = 0x01;
	xenon_smc_send_message(buf);
}

void xenon_smc_query_sensors(uint16_t *data)
{
	uint8_t buf[16] = {SMC_CMD_SENSORS};
	xenon_smc_send_message(buf);
	xenon_smc_receive_response(buf);
	int i;
	for (i = 0; i < 4; ++i)
		*data++ = (buf[i * 2 + 1] | (buf[i * 2 + 2] << 8));
}

int xenon_smc_read_avpack(void)
{
	uint8_t buf[16] = {SMC_CMD_AVPACK};
	xenon_smc_send_message(buf);
	xenon_smc_receive_response(buf);
	return buf[1];
}

void xenon_smc_set_fan_algorithm(int algorithm)
{
	uint8_t buf[16] = {SMC_CMD_FAN_ALGO, algorithm};
	xenon_smc_send_message(buf);
}
