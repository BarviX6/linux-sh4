/*
 * arch/sh/kernel/cpu/clock.c - SuperH clock framework
 *
 *  Copyright (C) 2005, 2006, 2007  Paul Mundt
 *
 * This clock framework is derived from the OMAP version by:
 *
 *	Copyright (C) 2004 - 2005 Nokia Corporation
 *	Written by Tuukka Tikkanen <tuukka.tikkanen@elektrobit.com>
 *
 *  Modified for omap shared clock framework by Tony Lindgren <tony@atomide.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/kref.h>
#include <linux/kobject.h>
#include <linux/sysdev.h>
#include <linux/seq_file.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <asm/clock.h>
#include <asm/timer.h>

static LIST_HEAD(clock_list);
static DEFINE_SPINLOCK(clock_lock);
static DEFINE_MUTEX(clock_list_sem);

static void propagate_rate(struct clk *clk)
{
	struct clk *clkp;

	list_for_each_entry(clkp, &clock_list, node) {
		if (likely(clkp->parent != clk))
			continue;
		if (likely(clkp->ops && clkp->ops->recalc))
			clkp->ops->recalc(clkp);
		if (unlikely(clkp->flags & CLK_RATE_PROPAGATES))
			propagate_rate(clkp);
	}
}

static int __clk_enable(struct clk *clk)
{
	kref_get(&clk->kref);

	if (clk->flags & CLK_ALWAYS_ENABLED)
		return 0;

	if (likely(clk->ops && clk->ops->enable))
		clk->ops->enable(clk);

	return 0;
}

int clk_enable(struct clk *clk)
{
	unsigned long flags;
	int ret;

	if (!clk)
		return -EINVAL;

	clk_enable(clk->parent);

	spin_lock_irqsave(&clock_lock, flags);
	ret = __clk_enable(clk);
	spin_unlock_irqrestore(&clock_lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(clk_enable);

static void clk_kref_release(struct kref *kref)
{
	/* Nothing to do */
}

static void __clk_disable(struct clk *clk)
{
	int count = kref_put(&clk->kref, clk_kref_release);

	if (clk->flags & CLK_ALWAYS_ENABLED)
		return;

	if (!count) {	/* count reaches zero, disable the clock */
		if (likely(clk->ops && clk->ops->disable))
			clk->ops->disable(clk);
	}
}

void clk_disable(struct clk *clk)
{
	unsigned long flags;

	if (!clk)
		return;

	spin_lock_irqsave(&clock_lock, flags);
	__clk_disable(clk);
	spin_unlock_irqrestore(&clock_lock, flags);

	clk_disable(clk->parent);
}
EXPORT_SYMBOL_GPL(clk_disable);

int clk_register(struct clk *clk)
{
	mutex_lock(&clock_list_sem);

	list_add(&clk->node, &clock_list);
	INIT_LIST_HEAD(&clk->childs);
	if (clk->parent)
		list_add(&clk->childs_node, &clk->parent->childs);
	kref_init(&clk->kref);

	mutex_unlock(&clock_list_sem);

	if (clk->ops && clk->ops->init)
		clk->ops->init(clk);

	if (clk->flags & CLK_ALWAYS_ENABLED) {
		pr_debug( "Clock '%s' is ALWAYS_ENABLED\n", clk->name);
		if (clk->ops && clk->ops->enable)
			clk->ops->enable(clk);
		pr_debug( "Enabled.");
	}

	return 0;
}
EXPORT_SYMBOL_GPL(clk_register);

void clk_unregister(struct clk *clk)
{
	mutex_lock(&clock_list_sem);
	list_del(&clk->node);
	if (clk->parent)
		list_del(&clk->childs_node);
	mutex_unlock(&clock_list_sem);
}
EXPORT_SYMBOL_GPL(clk_unregister);

unsigned long clk_get_rate(struct clk *clk)
{
	return clk->rate;
}
EXPORT_SYMBOL_GPL(clk_get_rate);

int clk_set_rate(struct clk *clk, unsigned long rate)
{
	return clk_set_rate_ex(clk, rate, 0);
}
EXPORT_SYMBOL_GPL(clk_set_rate);

int clk_set_rate_ex(struct clk *clk, unsigned long rate, int algo_id)
{
	int ret = -EOPNOTSUPP;

	if (likely(clk->ops && clk->ops->set_rate)) {
		unsigned long flags;

		spin_lock_irqsave(&clock_lock, flags);
		ret = clk->ops->set_rate(clk, rate, algo_id);
		spin_unlock_irqrestore(&clock_lock, flags);
	}

	if (unlikely(clk->flags & CLK_RATE_PROPAGATES))
		propagate_rate(clk);

	return ret;
}
EXPORT_SYMBOL_GPL(clk_set_rate_ex);

void clk_recalc_rate(struct clk *clk)
{
	if (likely(clk->ops && clk->ops->recalc)) {
		unsigned long flags;

		spin_lock_irqsave(&clock_lock, flags);
		clk->ops->recalc(clk);
		spin_unlock_irqrestore(&clock_lock, flags);
	}

	if (unlikely(clk->flags & CLK_RATE_PROPAGATES))
		propagate_rate(clk);
}
EXPORT_SYMBOL_GPL(clk_recalc_rate);

int clk_set_parent(struct clk *clk, struct clk *parent)
{
	int ret = -EINVAL;
	struct clk *old;

	if (!parent || !clk)
		return ret;

	old = clk->parent;
	if (likely(clk->ops && clk->ops->set_parent)) {
		unsigned long flags;
		spin_lock_irqsave(&clock_lock, flags);
		ret = clk->ops->set_parent(clk, parent);
		spin_unlock_irqrestore(&clock_lock, flags);
		clk->parent = (ret ? old : parent);
	}

	if (unlikely(clk->flags & CLK_RATE_PROPAGATES))
		propagate_rate(clk);
	return ret;
}
EXPORT_SYMBOL_GPL(clk_set_parent);

struct clk *clk_get_parent(struct clk *clk)
{
	return clk->parent;
}
EXPORT_SYMBOL_GPL(clk_get_parent);

long clk_round_rate(struct clk *clk, unsigned long rate)
{
	if (likely(clk->ops && clk->ops->round_rate)) {
		unsigned long flags, rounded;

		spin_lock_irqsave(&clock_lock, flags);
		rounded = clk->ops->round_rate(clk, rate);
		spin_unlock_irqrestore(&clock_lock, flags);

		return rounded;
	}

	return clk_get_rate(clk);
}
EXPORT_SYMBOL_GPL(clk_round_rate);

int clk_observe(struct clk *clk, unsigned long *div)
{
	int ret = -EINVAL;
	if (!clk)
		return ret;
	if (likely(clk->ops && clk->ops->observe))
		ret = clk->ops->observe(clk, div);
	return ret;
}
EXPORT_SYMBOL_GPL(clk_observe);

/*
 * Returns a clock. Note that we first try to use device id on the bus
 * and clock name. If this fails, we try to use clock name only.
 */
struct clk *clk_get(struct device *dev, const char *id)
{
	struct clk *p, *clk = ERR_PTR(-ENOENT);
	int idno;

	if (dev == NULL || dev->bus != &platform_bus_type)
		idno = -1;
	else
		idno = to_platform_device(dev)->id;

	mutex_lock(&clock_list_sem);
	list_for_each_entry(p, &clock_list, node) {
		if (p->id == idno &&
		    strcmp(id, p->name) == 0 && try_module_get(p->owner)) {
			clk = p;
			goto found;
		}
	}

	list_for_each_entry(p, &clock_list, node) {
		if (strcmp(id, p->name) == 0 && try_module_get(p->owner)) {
			clk = p;
			break;
		}
	}

found:
	mutex_unlock(&clock_list_sem);

	return clk;
}
EXPORT_SYMBOL_GPL(clk_get);

void clk_put(struct clk *clk)
{
	if (clk && !IS_ERR(clk))
		module_put(clk->owner);
}
EXPORT_SYMBOL_GPL(clk_put);

static int show_clocks(char *buf, char **start, off_t off,
		       int len, int *eof, void *data)
{
	struct clk *clk;
	char *p = buf;

	list_for_each_entry_reverse(clk, &clock_list, node) {
		unsigned long rate = clk_get_rate(clk);

		p += sprintf(p, "%-12s\t: %ld.%02ldMHz\t%s\n", clk->name,
			     rate / 1000000, (rate % 1000000) / 10000,
			     ((clk->flags & CLK_ALWAYS_ENABLED) ||
			      (atomic_read(&clk->kref.refcount) != 1)) ?
			     "enabled" : "disabled");
	}

	return p - buf;
}

#ifdef CONFIG_PM
static int clks_sysdev_suspend(struct sys_device *dev, pm_message_t state)
{
	static pm_message_t prev_state;
	struct clk *clkp;

	switch (state.event) {
	case PM_EVENT_ON:
		/* Resumeing from hibernation */
		if (prev_state.event == PM_EVENT_FREEZE) {
			list_for_each_entry(clkp, &clock_list, node)
				if (likely(clkp->ops)) {
					unsigned long rate = clkp->rate;

					if (likely(clkp->ops->set_parent))
						clkp->ops->set_parent(clkp,
							clkp->parent);
					if (likely(clkp->ops->set_rate))
						clkp->ops->set_rate(clkp,
							rate, NO_CHANGE);
					else if (likely(clkp->ops->recalc))
						clkp->ops->recalc(clkp);
					}
		}
		break;
	case PM_EVENT_FREEZE:
		break;
	case PM_EVENT_SUSPEND:
		break;
	}

	prev_state = state;
	return 0;
}

static int clks_sysdev_resume(struct sys_device *dev)
{
	return clks_sysdev_suspend(dev, PMSG_ON);
}

static struct sysdev_class clks_sysdev_class = {
	.name = "clks",
};

static struct sysdev_driver clks_sysdev_driver = {
	.suspend = clks_sysdev_suspend,
	.resume = clks_sysdev_resume,
};

static struct sys_device clks_sysdev_dev = {
	.cls = &clks_sysdev_class,
};

static int __init clk_sysdev_init(void)
{
	sysdev_class_register(&clks_sysdev_class);
	sysdev_driver_register(&clks_sysdev_class, &clks_sysdev_driver);
	sysdev_register(&clks_sysdev_dev);

	return 0;
}
subsys_initcall(clk_sysdev_init);
#endif

static int __init clk_proc_init(void)
{
	struct proc_dir_entry *p;
	p = create_proc_read_entry("clocks", S_IRUSR, NULL,
				   show_clocks, NULL);
	if (unlikely(!p))
		return -EINVAL;

	return 0;
}
subsys_initcall(clk_proc_init);

int clk_for_each(int (*fn)(struct clk *clk, void *data), void *data)
{
	struct clk *clkp;
	int result = 0;

	if (!fn)
		return -1;

	mutex_lock(&clock_list_sem);
	list_for_each_entry(clkp, &clock_list, node) {
		result |= fn(clkp, data);
		}
	mutex_unlock(&clock_list_sem);
	return result;
}
