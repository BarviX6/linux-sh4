/*
 * STx7141 SH-4 Setup
 *
 * Copyright (C) 2008 STMicroelectronics Limited
 * Author: Stuart Menefy <stuart.menefy@st.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/stm/platform.h>
#include <asm/irq-ilc.h>



/* SH4-only resources ----------------------------------------------------- */

/* This is the eSTB ILC3 */
static struct platform_device ilc3_device = {
	.name		= "ilc3",
	.id		= 0,
	.num_resources	= 1,
	.resource	= (struct resource[]) {
		{
			.start	= 0xfd120000,
			.end	= 0xfd120000 + 0x900,
			.flags	= IORESOURCE_MEM
		}
	},
	.dev.platform_data = &(struct stm_plat_ilc3_data) {
		.default_priority = 7,
		.num_input = ILC_NR_IRQS,
		.num_output = 80,
		.first_irq = ILC_FIRST_IRQ,
		.cpu_irq = (int[]){ ILC_FIRST_IRQ-1, -1 },
	},
};

static struct platform_device comms_ilc_device = {
	.name		= "ilc3",
	.id		= 1,
	.dev.platform_data = &(struct stm_plat_ilc3_data) {
		.default_priority = 7,
		.num_input = COMMS_ILC_NR_IRQS,
		.num_output = 16,
		.first_irq = COMMS_ILC_FIRST_IRQ,
		.cpu_irq = (int[]){ -1 },
		},
	.num_resources  = 1,
	.resource	= (struct resource[]) {
		{
			.start  = 0xfd000000,
			.end    = 0xfd000000 + 0x900,
			.flags  = IORESOURCE_MEM
		},
	},
};

#include "stm-tmu.h"

static struct platform_device *stx7141_sh4_devices[] __initdata = {
	&ilc3_device,
	&comms_ilc_device,
	&tmu0_device,
	&tmu1_device,
	&tmu2_device,
};

static int __init stx7141_sh4_devices_setup(void)
{
	return platform_add_devices(stx7141_sh4_devices,
			ARRAY_SIZE(stx7141_sh4_devices));
}
arch_initcall(stx7141_sh4_devices_setup);

static struct platform_device *stx7141_sh4_early_devices[] __initdata = {
	&tmu0_device,
	&tmu1_device,
	&tmu2_device,	
};

void __init plat_early_device_setup(void)
{
	early_platform_add_devices(stx7141_sh4_early_devices,
				   ARRAY_SIZE(stx7141_sh4_early_devices));
}



/* Interrupt initialisation ----------------------------------------------- */

enum {
	UNUSED = 0,

	/* interrupt sources */
	IRL0, IRL1, IRL2, IRL3, /* only IRLM mode described here */
	TMU0, TMU1, TMU2, WDT, HUDI,
};

static struct intc_vect vectors[] = {
	INTC_VECT(TMU0, 0x400), INTC_VECT(TMU1, 0x420),
	INTC_VECT(TMU2, 0x440), INTC_VECT(TMU2, 0x460),
	INTC_VECT(WDT, 0x560),
	INTC_VECT(HUDI, 0x600),
};

static struct intc_prio_reg prio_registers[] = {
					   /*   15-12, 11-8,  7-4,   3-0 */
	{ 0xffd00004, 0, 16, 4, /* IPRA */     { TMU0, TMU1, TMU2,       } },
	{ 0xffd00008, 0, 16, 4, /* IPRB */     {  WDT,    0,    0,     0 } },
	{ 0xffd0000c, 0, 16, 4, /* IPRC */     {    0,    0,    0,  HUDI } },
	{ 0xffd00010, 0, 16, 4, /* IPRD */     { IRL0, IRL1,  IRL2, IRL3 } },
};

static DECLARE_INTC_DESC(intc_desc, "stx7141", vectors, NULL,
			 NULL, prio_registers, NULL);

void __init plat_irq_setup(void)
{
	unsigned long intc2_base = (unsigned long)ioremap(0xfe001000, 0x400);

	register_intc_controller(&intc_desc);

	/* Enable the INTC2 */
	writel(7, intc2_base + 0x300);	/* INTPRI00 */
	writel(1, intc2_base + 0x360);	/* INTMSKCLR00 */

}
