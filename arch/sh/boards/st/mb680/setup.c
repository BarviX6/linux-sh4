/*
 * arch/sh/boards/st/mb680/setup.c
 *
 * Copyright (C) 2008 STMicroelectronics Limited
 * Author: Stuart Menefy (stuart.menefy@st.com)
 *
 * May be copied or modified under the terms of the GNU General Public
 * License.  See linux/COPYING for more information.
 *
 * STMicroelectronics STx7105 Mboard support.
 */

#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/leds.h>
#include <linux/stm/pio.h>
#include <linux/stm/soc.h>
#include <linux/stm/emi.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/physmap.h>
#include <linux/mtd/partitions.h>
#include <linux/phy.h>
#include <linux/gpio_keys.h>
#include <linux/input.h>
#include <asm/irq-ilc.h>
#include <asm/irl.h>
#include <asm/io.h>
#include "../common/epld.h"

static int ascs[2] __initdata = { 2, 3 };

static void __init mb680_setup(char** cmdline_p)
{
	printk("STMicroelectronics STx7105 Mboard initialisation\n");

	stx7105_early_device_init();
	stx7105_configure_asc(ascs, 2, 0);
}

static struct plat_stm_pwm_data pwm_private_info = {
	.flags		= PLAT_STM_PWM_OUT0,
};

static struct plat_ssc_data ssc_private_info = {
	.capability  =
		ssc0_has(SSC_I2C_CAPABILITY) |
		ssc1_has(SSC_I2C_CAPABILITY) |
		ssc2_has(SSC_I2C_CAPABILITY) |
		ssc3_has(SSC_I2C_CAPABILITY),
	.routing =
		SSC3_SCLK_PIO3_6 | SSC3_MTSR_PIO3_7 | SSC3_MRST_PIO3_7,
};

static struct platform_device mb680_leds = {
	.name = "leds-gpio",
	.id = -1,
	.dev.platform_data = &(struct gpio_led_platform_data) {
		.num_leds = 2,
		.leds = (struct gpio_led[]) {
			{
				.name = "LD5",
				.default_trigger = "heartbeat",
				.gpio = stpio_to_gpio(2, 4),
			},
			{
				.name = "LD6",
				.gpio = stpio_to_gpio(2, 3),
			},
		},
	},
};

static struct plat_stmmacphy_data phy_private_data = {
	/* National Semiconductor DP83865 */
	.bus_id = 0,
	.phy_addr = 1,
	.phy_mask = 0,
	.interface = PHY_INTERFACE_MODE_MII,
};

static struct platform_device mb680_phy_device = {
	.name		= "stmmacphy",
	.id		= 0,
	.num_resources	= 1,
	.resource	= (struct resource[]) {
		{
			.name	= "phyirq",
			.start	= -1,/*FIXME, should be ILC_EXT_IRQ(6), */
			.end	= -1,
			.flags	= IORESOURCE_IRQ,
		},
	},
	.dev = {
		.platform_data = &phy_private_data,
	}
};

static struct platform_device *mb680_devices[] __initdata = {
	&mb680_leds,
	&mb680_phy_device,
};

static int __init device_init(void)
{
#if 0
	stx7105_configure_pwm(&pwm_private_info);
#endif
	stx7105_configure_ssc(&ssc_private_info);

	/*
	 * Note that USB port configuration depends on jumper
	 * settings:
	 *		  PORT 0  SW		PORT 1	SW
	 *		+----------------------------------------
	 * OC	normal	|  4[4]	J5A 2-3		 4[6]	J10A 2-3
	 *	alt	| 12[5]	J5A 1-2		14[6]	J10A 1-2
	 * PWR	normal	|  4[5]	J5B 2-3		 4[7]	J10B 2-3
	 *	alt	| 12[6]	J5B 1-2		14[7]	J10B 1-2
	 */

	stx7105_configure_usb(0, 1, 0, 0, 1, 0);
	stx7105_configure_usb(1, 1, 0, 0, 1, 0);
	stx7105_configure_ethernet(0, 0, 0, 1, 0, 0);
#if 0
        stx7105_configure_lirc();
#endif
#if 0
	stx7200_configure_pata(1, ILC_IRQ(6));	/* irq_ilc_ext_in[2] */
#endif

	/* Configure BANK2 for the db641 STEM card */
	emi_bank_configure(2, (unsigned long[4]){ 0x041086f1, 0x0e024400,
				0x0e024400, 0 });

	return platform_add_devices(mb680_devices, ARRAY_SIZE(mb680_devices));
}
arch_initcall(device_init);

static void __iomem *mb680_ioport_map(unsigned long port, unsigned int size)
{
	/* However picking somewhere safe isn't as easy as you might think.
	 * I used to use external ROM, but that can cause problems if you are
	 * in the middle of updating Flash. So I'm now using the processor core
	 * version register, which is guaranted to be available, and non-writable.
	 */
	return (void __iomem *)CCN_PVR;
}

static void __init mb680_init_irq(void)
{
}

struct sh_machine_vector mv_mb680 __initmv = {
	.mv_name		= "mb680",
	.mv_setup		= mb680_setup,
	.mv_nr_irqs		= NR_IRQS,
	.mv_init_irq		= mb680_init_irq,
	.mv_ioport_map		= mb680_ioport_map,
};
