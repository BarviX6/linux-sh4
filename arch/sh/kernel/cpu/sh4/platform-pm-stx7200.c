/*
 * Platform PM Capability - STx7200
 *
 * Copyright (C) 2009 STMicroelectronics Limited
 * Author: Francesco M. Virlinzi <francesco.virlinzi@st.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/stm/pm.h>
#ifdef CONFIG_PM
static int
usb_pwr_dwn(struct platform_device *pdev, int host_phy, int pwd)
{
	static struct sysconf_field *sc[3];
	int port = pdev->id;

	/* Power up port */
	if (!sc[port])
		sc[port] = sysconf_claim(SYS_CFG, 22, 3+port, 3+port,
				"usb pwr");

	sysconf_write(sc[port], (pwd ? 1 : 0));

	return 0;
}

static int
usb_pwr_ack(struct platform_device *pdev, int host_phy, int ack)
{
	static struct sysconf_field *sc[3];
	int port = pdev->id;
	if (!sc[port])
		sc[port] = sysconf_claim(SYS_STA, 13, 2+port, 2+port,
					"usb ack");

	while (sysconf_read(sc[port]) != ack);

	return 0;
}

static int
usb_sw_reset(struct platform_device *dev, int host_phy)
{
	/* it seems there is no reset on this platform... */
	return 0;
}

static int
emi_pwd_dwn_req(struct platform_device *pdev, int host_phy, int pwd)
{
	static struct sysconf_field *sc;
	if (!sc)
		sc = sysconf_claim(SYS_CFG, 32, 1, 1, "emi pwr req");

	sysconf_write(sc, (pwd ? 1 : 0));
	return 0;
}

static int
emi_pwd_dwn_ack(struct platform_device *pdev, int host_phy, int ack)
{
	static struct sysconf_field *sc;
	if (!sc)
		sc = sysconf_claim(SYS_STA, 8, 1, 1, "emi pwr ack");
/*	while (sysconf_read(sc) != ack);*/
	mdelay(50);
	return 0;
}

static struct platform_device_pm stx7200_pm_devices[] = {
pm_plat_name("emi", NULL, emi_pwd_dwn_req, emi_pwd_dwn_ack, NULL),
pm_plat_dev(&st_usb[0], NULL, usb_pwr_dwn, usb_pwr_ack, usb_sw_reset),
pm_plat_dev(&st_usb[1], NULL, usb_pwr_dwn, usb_pwr_ack, usb_sw_reset),
pm_plat_dev(&st_usb[2], NULL, usb_pwr_dwn, usb_pwr_ack, usb_sw_reset),
};
#endif
