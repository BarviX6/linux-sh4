/*
 * -------------------------------------------------------------------------
 * <linux_root>/arch/sh/kernel/cpu/sh4/suspend-stx7141.c
 * -------------------------------------------------------------------------
 * Copyright (C) 2009  STMicroelectronics
 * Author: Francesco M. Virlinzi  <francesco.virlinzi@st.com>
 *
 * May be copied or modified under the terms of the GNU General Public
 * License V.2 ONLY.  See linux/COPYING for more information.
 *
 * ------------------------------------------------------------------------- */

#include <linux/init.h>
#include <linux/suspend.h>
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/irqflags.h>
#include <linux/stm/pm.h>
#include <linux/stm/sysconf.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/pm.h>
#include <asm/irq-ilc.h>

#ifdef CONFIG_PM_DEBUG
#include <linux/stm/pio.h>
#endif

#include "./soc-stx7141.h"

#define _SYS_STA4		(7)
#define _SYS_STA4_MASK		(8)
#define _SYS_CFG11		(9)
#define _SYS_CFG11_MASK		(10)
#define _SYS_CFG38		(5)
#define _SYS_CFG38_MASK		(6)

/* *************************
 * STANDBY INSTRUCTION TABLE
 * *************************
 */
static unsigned long stx7141_standby_table[] __cacheline_aligned = {
IMMEDIATE_DEST(0x1f),
/* reduces the st40 frequency */
CLK_STORE(CKGA_OSC_DIV_CFG(4)),
/* reduces the clk_ic */
CLK_STORE(CKGA_OSC_DIV_CFG(0x0)),

CLK_POKE(CKGA_OSC_DIV_CFG(10), 29),/* clk_ic_if_100 @ 1 MHz to be safe for lirc */
/* 2. Move all the clock on OSC */
CLK_POKE(CKGA_CLKOPSRC_SWITCH_CFG(0x0), 0),

 /* END. */
_END(),

DATA_LOAD(0x0),
CLK_STORE(CKGA_CLKOPSRC_SWITCH_CFG(0x0)),
DATA_LOAD(0x1),
CLK_STORE(CKGA_OSC_DIV_CFG(0x0)),
DATA_LOAD(0x2),
CLK_STORE(CKGA_OSC_DIV_CFG(10)),
 /* END. */
_END()
};

/* *********************
 * MEM INSTRUCTION TABLE
 * *********************
 */
static unsigned long stx7141_mem_table[] __cacheline_aligned = {
/* 1. Enables the DDR self refresh mode */
DATA_OR_LONG(_SYS_CFG38, _SYS_CFG38_MASK),
/* waits until the ack bit is zero */
DATA_WHILE_NEQ(_SYS_STA4, _SYS_STA4_MASK, _SYS_STA4_MASK),
/* 1.1 Turn-off the ClockGenD */
DATA_OR_LONG(_SYS_CFG11, _SYS_CFG11_MASK),

IMMEDIATE_DEST(0x1f),
/* reduces OSC_st40 */
CLK_STORE(CKGA_OSC_DIV_CFG(4)),
/* reduces OSC_clk_ic */
CLK_STORE(CKGA_OSC_DIV_CFG(0x0)),
/* reduces OSC_clk_ic_if_200 */
CLK_STORE(CKGA_OSC_DIV_CFG(17)),

CLK_POKE(CKGA_OSC_DIV_CFG(10), 29),/* clk_ic_if_100 @ 1 MHz to be safe for lirc */
/* 2. Move all the clock on OSC */
IMMEDIATE_DEST(0x0),
CLK_STORE(CKGA_CLKOPSRC_SWITCH_CFG(0x0)),
CLK_STORE(CKGA_CLKOPSRC_SWITCH_CFG(0x1)),
/* PLLs in power down */
CLK_OR_LONG(CKGA_POWER_CFG, 0x3),
 /* END. */
_END(),

/* Turn-on the PLLs */
CLK_AND_LONG(CKGA_POWER_CFG, ~3),
/* 1. Turn-on the LMI ClocksGenD */
DATA_AND_NOT_LONG(_SYS_CFG11, _SYS_CFG11_MASK),
/* 2. Disables the DDR self refresh mode */
DATA_AND_NOT_LONG(_SYS_CFG38, _SYS_CFG38_MASK),
/* waits until the ack bit is zero */
DATA_WHILE_EQ(_SYS_STA4, _SYS_STA4_MASK, _SYS_STA4_MASK),

DATA_LOAD(0x0),
CLK_STORE(CKGA_CLKOPSRC_SWITCH_CFG(0x0)),
DATA_LOAD(0x1),
CLK_STORE(CKGA_CLKOPSRC_SWITCH_CFG(0x1)),

DATA_LOAD(0x2),
CLK_STORE(CKGA_OSC_DIV_CFG(0x0)),

DATA_LOAD(0x3),
CLK_STORE(CKGA_OSC_DIV_CFG(10)),

DATA_LOAD(0x4),
CLK_STORE(CKGA_OSC_DIV_CFG(17)),
_DELAY(),
_DELAY(),
_DELAY(),
_END()
};

static unsigned long stx7141_wrt_table[16] __cacheline_aligned;

static int stx7141_suspend_prepare(suspend_state_t state)
{
	int ret = -EINVAL;
	pm_message_t pms = {.event = PM_EVENT_SUSPEND, };
	emi_pm_state(pms);
	clk_pm_state(pms);
	sysconf_pm_state(pms);

	switch (state) {
	case PM_SUSPEND_STANDBY:
		stx7141_wrt_table[0] = /* swith config */
		   ioread32(CLOCKGENA_BASE_ADDR + CKGA_CLKOPSRC_SWITCH_CFG(0));
		stx7141_wrt_table[1] = /* clk_ic */
		   ioread32(CLOCKGENA_BASE_ADDR + CKGA_OSC_DIV_CFG(0));
		stx7141_wrt_table[2] = /* clk_ic_if_100 */
		    ioread32(CLOCKGENA_BASE_ADDR + CKGA_OSC_DIV_CFG(10));
		ret = 0;
		break;
	case PM_SUSPEND_MEM:
		stx7141_wrt_table[0] = /* swith config */
		   ioread32(CLOCKGENA_BASE_ADDR + CKGA_CLKOPSRC_SWITCH_CFG(0));
		stx7141_wrt_table[1] = /* swith config */
		   ioread32(CLOCKGENA_BASE_ADDR + CKGA_CLKOPSRC_SWITCH_CFG(1));
		stx7141_wrt_table[2] = /* clk_ic */
		   ioread32(CLOCKGENA_BASE_ADDR + CKGA_OSC_DIV_CFG(0));
		stx7141_wrt_table[3] = /* clk_ic_if_100 */
		    ioread32(CLOCKGENA_BASE_ADDR + CKGA_OSC_DIV_CFG(10));
		stx7141_wrt_table[4] = /* clk_ic_if_200 */
		    ioread32(CLOCKGENA_BASE_ADDR + CKGA_OSC_DIV_CFG(17));
		ret = 0;
		break;
	}
	return ret;
}

static int stx7141_suspend_valid(suspend_state_t state)
{
	switch (state) {
	case PM_SUSPEND_STANDBY:
	case PM_SUSPEND_MEM:
		return 1;
	};
	return 0;
}

/*
 * The xxxx_finish function is called after the resume
 * sysdev devices (i.e.: timer, cpufreq)
 * But it isn't a big issue in our platform
 */
static int stx7141_suspend_finish(suspend_state_t state)
{
	pm_message_t pms = {.event = PM_EVENT_ON, };
	sysconf_pm_state(pms);
	clk_pm_state(pms);
	emi_pm_state(pms);
	return 0;
}

static unsigned long stx7141_iomem[2] __cacheline_aligned = {
		stx7141_wrt_table,
		CLOCKGENA_BASE_ADDR};

static int stx7141_evttoirq(unsigned long evt)
{
	return ilc2irq(evt);
}

int __init suspend_platform_setup(struct sh4_suspend_t *st40data)
{
	struct sysconf_field *sc;
#ifdef CONFIG_PM_DEBUG
	/* route the sh4/2  clock frequenfy */
	iowrite32(0xc, CLOCKGENA_BASE_ADDR + CKGA_CLKOBS_MUX1_CFG);
	stpio_request_set_pin(3, 2, "clkA dbg", STPIO_ALT_OUT, 1);
	sc = sysconf_claim(SYS_CFG, 19, 22, 23, "clkA dbg");
	sysconf_write(sc, 11);
#endif
	st40data->iobase = stx7141_iomem;
	st40data->ops.valid = stx7141_suspend_valid;
	st40data->ops.finish = stx7141_suspend_finish;
	st40data->ops.prepare = stx7141_suspend_prepare;

	st40data->evt_to_irq = stx7141_evttoirq;

	st40data->stby_tbl = (unsigned long)stx7141_standby_table;
	st40data->stby_size = DIV_ROUND_UP(
		ARRAY_SIZE(stx7141_standby_table)*sizeof(long), L1_CACHE_BYTES);

	st40data->mem_tbl = (unsigned long)stx7141_mem_table;
	st40data->mem_size = DIV_ROUND_UP(
		ARRAY_SIZE(stx7141_mem_table)*sizeof(long), L1_CACHE_BYTES);

	st40data->wrt_tbl = (unsigned long)stx7141_wrt_table;
        st40data->wrt_size = DIV_ROUND_UP(
		ARRAY_SIZE(stx7141_wrt_table) * sizeof(long), L1_CACHE_BYTES);

	sc = sysconf_claim(SYS_CFG, 38, 20, 20, "pm");
	stx7141_wrt_table[_SYS_CFG38]      = (unsigned long)sysconf_address(sc);
	stx7141_wrt_table[_SYS_CFG38_MASK] = sysconf_mask(sc);

	sc = sysconf_claim(SYS_CFG, 11, 12, 12, "pm");
	stx7141_wrt_table[_SYS_CFG11]      = (unsigned long)sysconf_address(sc);
	stx7141_wrt_table[_SYS_CFG11_MASK] = sysconf_mask(sc);

	sc = sysconf_claim(SYS_STA, 4, 0, 0, "pm");
	stx7141_wrt_table[_SYS_STA4]      = (unsigned long)sysconf_address(sc);
	stx7141_wrt_table[_SYS_STA4_MASK] = sysconf_mask(sc);

	return 0;
}
