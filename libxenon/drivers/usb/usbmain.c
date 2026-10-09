/*  *********************************************************************
    *  Broadcom Common Firmware Environment (CFE)
    *
    *  Main Module				File: usbmain.c
    *
    *  Main module that invokes the top of the USB stack from CFE.
    *
    *  Author:  Mitch Lichtenberg
    *
    *********************************************************************
    *
    *  Copyright 2000,2001,2002,2003
    *  Broadcom Corporation. All rights reserved.
    *
    *  This software is furnished under license and may be used and
    *  copied only in accordance with the following terms and
    *  conditions.  Subject to these conditions, you may download,
    *  copy, install, use, modify and distribute modified or unmodified
    *  copies of this software in source and/or binary form.  No title
    *  or ownership is transferred hereby.
    *
    *  1) Any source code used, modified or distributed must reproduce
    *     and retain this copyright notice and list of conditions
    *     as they appear in the source file.
    *
    *  2) No right is granted to use any trade name, trademark, or
    *     logo of Broadcom Corporation.  The "Broadcom Corporation"
    *     name may not be used to endorse or promote products derived
    *     from this software without the prior written permission of
    *     Broadcom Corporation.
    *
    *  3) THIS SOFTWARE IS PROVIDED "AS-IS" AND ANY EXPRESS OR
    *     IMPLIED WARRANTIES, INCLUDING BUT NOT LIMITED TO, ANY IMPLIED
    *     WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
    *     PURPOSE, OR NON-INFRINGEMENT ARE DISCLAIMED. IN NO EVENT
    *     SHALL BROADCOM BE LIABLE FOR ANY DAMAGES WHATSOEVER, AND IN
    *     PARTICULAR, BROADCOM SHALL NOT BE LIABLE FOR DIRECT, INDIRECT,
    *     INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    *     (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
    *     GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
    *     BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
    *     OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
    *     TORT (INCLUDING NEGLIGENCE OR OTHERWISE), EVEN IF ADVISED OF
    *     THE POSSIBILITY OF SUCH DAMAGE.
    ********************************************************************* */


#include <nocfe/cfe.h>

#include "usbmain.h"
#include "usbchap9.h"
#include "usbd.h"
#include "pci/io.h"
#include <usb/tinyehci/tinyehci.h>


/*  *********************************************************************
    *  Externs
    ********************************************************************* */

extern usb_hcdrv_t ohci_driver;			/* OHCI Driver dispatch */

extern int ohcidebug;				/* OHCI debug control */
extern int usb_noisy;				/* USBD debug control */

int ui_init_usbcmds(void);			/* forward */
void kmem_init(void);

/*  *********************************************************************
    *  Globals
    ********************************************************************* */

/*
 * We keep track of the pointers to USB buses in globals.
 * One entry in this array per USB bus (the Opti controller
 * on the SWARM has two functions, so it's two buses)
 */

#define USB_MAX_BUS 4
int usb_buscnt = 0;
usbbus_t *usb_buses[USB_MAX_BUS];


/*  *********************************************************************
    *  usb_cfe_timer(arg)
    *
    *  This routine is called periodically by CFE's timer routines
    *  to give the USB subsystem some time.  Basically we scan
    *  for work to do to manage configuration updates, and handle
    *  interrupts from the USB controllers.
    *
    *  Input parameters:
    *  	   arg - value we passed when the timer was initialized
    *  	          (not used)
    *
    *  Return value:
    *  	   nothing
    ********************************************************************* */

static void usb_cfe_timer(void *arg)
{
    int idx;
    static int in_poll = 0;

    /*
     * We sometimes call the timer routines in here, which calls
     * the polling loop.  This code is not reentrant, so
     * prevent us from running the interrupt routine or
     * bus daemon while we are already in there.
     */

    if (in_poll) return;

    /*
     * Do not allow nested "interrupts."
     */

    in_poll = 1;

    for (idx = 0; idx < usb_buscnt; idx++) {
	if (usb_buses[idx]) {
	    usb_poll(usb_buses[idx]);
	    usb_daemon(usb_buses[idx]);
	    }
	}

    /*
     * Okay to call polling again.
     */

    in_poll = 0;
}


/*  *********************************************************************
    *  usb_init_one_ohci(addr)
    *
    *  Initialize one USB controller.
    *
    *  Input parameters:
    *  	   addr - physical address of OHCI registers
    *
    *  Return value:
    *  	   0 if ok
    *  	   else error
    ********************************************************************* */
static int usb_init_one_ohci(uint32_t addr)
{
    usbbus_t *bus;
    int res;

    bus = UBCREATE(&ohci_driver, addr);

    if (bus == NULL) {
	printf("USB: Could not create OHCI driver structure for controller at 0x%08X\n",addr);
	return -1;
	}

    bus->ub_num = usb_buscnt;

    res = UBSTART(bus);

    if (res != 0) {
	printf("USB: Could not init OHCI controller at 0x%08X\n",addr);
	UBSTOP(bus);
	return -1;
	}
    else {
	usb_buses[usb_buscnt++] = bus;
	usb_initroot(bus);
	}

    return 0;
}

int usb_initialized = 0;

int usb_init(void)
{
    static int initdone = 0;

    if (initdone) {
		printf("USB has already been initialized.\n");
		return -1;
	}

	initdone = 1;

	usb_shutdown();

	// preinit (start from scratch)
		// OHCI
	write32(0xD0120044,0xed44);
	write32(0xD0128044,0xed44);
		// EHCI
	write32(0xD0121040,0x0C004020);
	write32(0xD0129040,0x0C004020);
	write32(0xD0121044,0x3C);
	write32(0xD0129044,0x3C);

	printf(" * Initialising USB EHCI...\n");
	EHCI_Init();

	printf(" * Initialising USB OHCI...\n");

	usb_buscnt = 0;

    kmem_init();

#if 0
	usb_noisy = 1;
	ohcidebug = 2;
#endif

	usb_init_one_ohci(0xea002000);
	usb_init_one_ohci(0xea004000);

//    cfe_bg_add(usb_cfe_timer,NULL);
	usb_initialized = 1;

	return 0;
}

void usb_shutdown(void)
{
	int n;

	// EHCI
		// disable interrupts
	write32(0xEA003028,0);
	write32(0xEA005028,0);
		// halt, then wait for HCHalted (USBSTS bit 12), at most 100 ms each
	write32(0xEA003020,0);
	write32(0xEA005020,0);
	for (n = 0; n < 100 && !(read32(0xEA003024) & (1 << 12)); n++)
		mdelay(1);
	for (n = 0; n < 100 && !(read32(0xEA005024) & (1 << 12)); n++)
		mdelay(1);

	// OHCI
		// disable interrupts
	write32(0xEA002014,1 << 31);
	write32(0xEA004014,1 << 31);
		// halt
	write32(0xEA002004,0);
	read32(0xEA002004);
	write32(0xEA004004,0);
	read32(0xEA004004);
}

void usb_do_poll(void)
{
	if (!usb_initialized)
		return;

	usb_cfe_timer(0);

	USBStorage_Init();
}
