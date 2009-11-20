/* ============================================================================
 *
 * drivers/net/stmmac/stmmac_main.c
 *
 * This is the driver for the MAC 10/100/1000 on-chip Ethernet controllers
 * (Synopsys IP).
 *
 * Author: Giuseppe Cavallaro <peppe.cavallaro@st.com>
 *
 * Copyright (C) 2007-2009 by STMicroelectronics
 *
 * Documentation available at:
 *  http://www.stlinux.com
 * Support available at:
 *  https://bugzilla.stlinux.com/
 * ===========================================================================*/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/skbuff.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/crc32.h>
#include <linux/mii.h>
#include <linux/phy.h>
#include <linux/if_vlan.h>
#include <linux/dma-mapping.h>
#include <linux/stm/soc.h>
#include "stmmac.h"

#define STMMAC_RESOURCE_NAME	"stmmaceth"
#define PHY_RESOURCE_NAME	"stmmacphy"

#undef STMMAC_DEBUG
/*#define STMMAC_DEBUG*/
#ifdef STMMAC_DEBUG
#define DBG(nlevel, klevel, fmt, args...) \
		((void)(netif_msg_##nlevel(priv) && \
		printk(KERN_##klevel fmt, ## args)))
#else
#define DBG(nlevel, klevel, fmt, args...) do { } while (0)
#endif

#undef STMMAC_RX_DEBUG
/*#define STMMAC_RX_DEBUG*/
#ifdef STMMAC_RX_DEBUG
#define RX_DBG(fmt, args...)  printk(fmt, ## args)
#else
#define RX_DBG(fmt, args...)  do { } while (0)
#endif

#undef STMMAC_XMIT_DEBUG
/*#define STMMAC_XMIT_DEBUG*/

#define STMMAC_ALIGN(x)	L1_CACHE_ALIGN(x)
#define STMMAC_IP_ALIGN NET_IP_ALIGN
#define JUMBO_LEN	9000

/* Module parameters */
#define TX_TIMEO (5*HZ)
static int watchdog = TX_TIMEO;
module_param(watchdog, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(watchdog, "Transmit timeout");

static int debug = -1;		/* -1: default, 0: no output, 16:  all */
module_param(debug, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Message Level (0: no output, 16: all)");

static int phyaddr = -1;
module_param(phyaddr, int, S_IRUGO);
MODULE_PARM_DESC(phyaddr, "Physical device address");

#define DMA_TX_SIZE 128
static int dma_txsize = DMA_TX_SIZE;
module_param(dma_txsize, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(dma_txsize, "Number of descriptors in the TX list");

#define DMA_RX_SIZE 128
static int dma_rxsize = DMA_RX_SIZE;
module_param(dma_rxsize, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(dma_rxsize, "Number of descriptors in the RX list");

static int flow_ctrl = FLOW_OFF;
module_param(flow_ctrl, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(flow_ctrl, "Flow control ability [on/off]");

static int pause = PAUSE_TIME;
module_param(pause, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(pause, "Flow Control Pause Time");

#define TC_DEFAULT 64
static int tc = TC_DEFAULT;
module_param(tc, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(tc, "DMA threshold control value");

#define RX_NO_COALESCE	1	/* Always interrupt on completion */
#define TX_NO_COALESCE	-1	/* No moderation by default */

/* It makes sense to combine interrupt coalescence when the timer is enabled
 * to avoid adverse effects on timing and make safe the TCP traffic.*/
static int rx_coalesce = RX_NO_COALESCE;
module_param(rx_coalesce, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(rx_coalesce, "Rx irq coalescence parameter");

static int tx_coalesce = TX_NO_COALESCE;
module_param(tx_coalesce, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(tx_coalesce, "Tx irq coalescence parameter");

/* Pay attention to tune this parameter; take care of both
 * hardware capability and network stabitily/performance impact.
 * Many tests showed that ~4ms latency seems to be good enough. */
#ifdef CONFIG_STMMAC_TIMER
#define DEFAULT_PERIODIC_RATE	256
static int tmrate = DEFAULT_PERIODIC_RATE;
module_param(tmrate, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(tmrate, "External timer freq. (default: 256Hz)");
#endif

#define DMA_BUFFER_SIZE	BUF_SIZE_2KiB
static int buf_sz = DMA_BUFFER_SIZE;
module_param(buf_sz, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(buf_sz, "DMA buffer size");

/* In case of Giga ETH, we can enable/disable the COE for the
 * transmit HW checksum computation.
 * Note that, if tx csum is off in HW, SG will be still supported. */
static int tx_coe = HW_CSUM;
module_param(tx_coe, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(tx_coe, "GMAC COE type 2 [on/off]");

static const u32 default_msg_level = (NETIF_MSG_DRV | NETIF_MSG_PROBE |
				      NETIF_MSG_LINK | NETIF_MSG_IFUP |
				      NETIF_MSG_IFDOWN | NETIF_MSG_TIMER);

static irqreturn_t stmmac_interrupt(int irq, void *dev_id);
static int stmmac_rx(struct net_device *dev, int limit);
static int stmmac_xmit(struct sk_buff *skb, struct net_device *dev);

/**
 * stmmac_init_coalescence - init the coalescence parameters
 * @gmac: to identify if the device; mac100 and gmac don't use the same tuning.
 * @mtu: to moderate the coalescence in case of oversized frames.
 * Description: initialises the coalescence parameters statically when
 *		use Timer optimisation.
 * These values have been set based on testing data as well as attempting
 * to minimize response time while increasing bulk throughput.
 * These parameters can also be tuned via sys and new values can be
 * used after reopening the interface (via ifconfig for example).
 * TODO: dynamic tuning.
 */
static void stmmac_init_coalescence(int gmac, int mtu)
{
#ifdef CONFIG_STMMAC_TIMER
	/* maybe, params passed through cmdline?!? Do not use the defaults
	 * values. */
	if ((rx_coalesce != RX_NO_COALESCE) ||
	    (tx_coalesce != TX_NO_COALESCE))
		return;

	if (gmac) {
		rx_coalesce = 32;
		tx_coalesce = 64;

		if (unlikely(mtu > ETH_DATA_LEN)) {
			/* Tests on oversized frames showed that it's better to
			 * reduce the coalescence values to get the max
			 * throughput. */
			rx_coalesce = 4;
			tx_coalesce = 4;
		}
	} else {
		rx_coalesce = 16;
		tx_coalesce = 32;
	}
#endif
}

/**
 * stmmac_verify_args - Check work parameters passed to the driver
 * Description: wrong parameters are replaced with the default values
 */
static void stmmac_verify_args(void)
{
	if (unlikely(watchdog < 0))
		watchdog = TX_TIMEO;
	if (unlikely(dma_rxsize < 0))
		dma_rxsize = DMA_RX_SIZE;
	if (unlikely(dma_txsize < 0))
		dma_txsize = DMA_TX_SIZE;
	if (unlikely(tx_coalesce >= (dma_txsize)))
		tx_coalesce = TX_NO_COALESCE;
	if (unlikely(rx_coalesce > (dma_rxsize)))
		rx_coalesce = RX_NO_COALESCE;
	if (unlikely((buf_sz < DMA_BUFFER_SIZE) || (buf_sz > BUF_SIZE_16KiB)))
		buf_sz = DMA_BUFFER_SIZE;
	if (unlikely(flow_ctrl > 1))
		flow_ctrl = FLOW_AUTO;
	else if (likely(flow_ctrl < 0))
		flow_ctrl = FLOW_OFF;
	if (unlikely((pause < 0) || (pause > 0xffff)))
		pause = PAUSE_TIME;

	return;
}

#if defined(STMMAC_XMIT_DEBUG) || defined(STMMAC_RX_DEBUG)
static void print_pkt(unsigned char *buf, int len)
{
	int j;
	pr_info("len = %d byte, buf addr: 0x%p", len, buf);
	for (j = 0; j < len; j++) {
		if ((j % 16) == 0)
			pr_info("\n %03x:", j);
		pr_info(" %02x", buf[j]);
	}
	pr_info("\n");
	return;
}
#endif

static inline u32 stmmac_tx_avail(struct stmmac_priv *priv)
{
	return priv->dirty_tx + priv->dma_tx_size - priv->cur_tx - 1;
}

/**
 * stmmac_adjust_link
 * @dev: net device structure
 * Description: it adjusts the link parameters.
 */
static void stmmac_adjust_link(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	struct phy_device *phydev = priv->phydev;
	unsigned long ioaddr = dev->base_addr;
	unsigned long flags;
	int new_state = 0;
	unsigned int fc = priv->flow_ctrl, pause_time = priv->pause;

	if (phydev == NULL)
		return;

	DBG(probe, DEBUG, "stmmac_adjust_link: called.  address %d link %d\n",
	    phydev->addr, phydev->link);

	spin_lock_irqsave(&priv->lock, flags);
	if (phydev->link) {
		u32 ctrl = readl(ioaddr + MAC_CTRL_REG);

		/* Now we make sure that we can be in full duplex mode.
		 * If not, we operate in half-duplex mode. */
		if (phydev->duplex != priv->oldduplex) {
			new_state = 1;
			if (!(phydev->duplex))
				ctrl &= ~priv->mac_type->hw.link.duplex;
			else
				ctrl |= priv->mac_type->hw.link.duplex;
			priv->oldduplex = phydev->duplex;
		}
		/* Flow Control operation */
		if (phydev->pause)
			priv->mac_type->ops->flow_ctrl(ioaddr, phydev->duplex,
						       fc, pause_time);

		if (phydev->speed != priv->speed) {
			new_state = 1;
			switch (phydev->speed) {
			case 1000:
				if (likely(priv->is_gmac))
					ctrl &= ~priv->mac_type->hw.link.port;
				break;
			case 100:
			case 10:
				if (priv->is_gmac) {
					ctrl |= priv->mac_type->hw.link.port;
					if (phydev->speed == SPEED_100) {
						ctrl |=
						    priv->mac_type->hw.link.
						    speed;
					} else {
						ctrl &=
						    ~(priv->mac_type->hw.
						      link.speed);
					}
				} else {
					ctrl &= ~priv->mac_type->hw.link.port;
				}
				priv->fix_mac_speed(priv->bsp_priv,
						    phydev->speed);
				break;
			default:
				if (netif_msg_link(priv))
					pr_warning("%s: Speed (%d) is not 10"
				       " or 100!\n", dev->name, phydev->speed);
				break;
			}

			priv->speed = phydev->speed;
		}

		writel(ctrl, ioaddr + MAC_CTRL_REG);

		if (!priv->oldlink) {
			new_state = 1;
			priv->oldlink = 1;
		}
	} else if (priv->oldlink) {
		new_state = 1;
		priv->oldlink = 0;
		priv->speed = 0;
		priv->oldduplex = -1;
	}

	if (new_state && netif_msg_link(priv))
		phy_print_status(phydev);

	spin_unlock_irqrestore(&priv->lock, flags);

	DBG(probe, DEBUG, "stmmac_adjust_link: exiting\n");
}

/**
 * stmmac_init_phy - PHY initialization
 * @dev: net device structure
 * Description: it initializes driver's PHY state, and attaches to the PHY.
 *  Return value:
 *  0 on success
 */
static int stmmac_init_phy(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	struct phy_device *phydev;
	char phy_id[BUS_ID_SIZE];	/* PHY to connect */
	char bus_id[BUS_ID_SIZE];

	priv->oldlink = 0;
	priv->speed = 0;
	priv->oldduplex = -1;

	if (priv->phy_addr == -1) {
		/* We don't have a PHY, so do nothing */
		return 0;
	}

	snprintf(bus_id, MII_BUS_ID_SIZE, "%x", priv->bus_id);
	snprintf(phy_id, BUS_ID_SIZE, PHY_ID_FMT, bus_id, priv->phy_addr);
	pr_debug("stmmac_init_phy:  trying to attach to %s\n", phy_id);

	phydev = phy_connect(dev, phy_id, &stmmac_adjust_link, 0,
			priv->phy_interface);

	if (IS_ERR(phydev)) {
		pr_err("%s: Could not attach to PHY\n", dev->name);
		return PTR_ERR(phydev);
	}

	/*
	 * Broken ST PHY HW is sometimes missing the pull-up resistor on the
	 * MDIO line, which results in reads to non-existent devices returning
	 * 0 rather than 0xffff. Catch this here and treat 0 as a non-existent
	 * device as well.
	 * Note: phydev->phy_id is the result of reading the UID PHY registers.
	 */
	if (phydev->phy_id == 0) {
		phy_disconnect(phydev);
		return -ENODEV;
	}
	pr_debug("stmmac_init_phy:  %s: attached to PHY (UID 0x%x)"
	       " Link = %d\n", dev->name, phydev->phy_id, phydev->link);

	priv->phydev = phydev;

	return 0;
}

/**
 * stmmac_mac_enable_rx
 * @dev: net device structure
 * Description: the function enables the RX MAC process
 */
static void stmmac_mac_enable_rx(struct net_device *dev)
{
	unsigned long ioaddr = dev->base_addr;
	u32 value = readl(ioaddr + MAC_CTRL_REG);

	/* set the RE (receive enable, bit 2) */
	value |= MAC_RNABLE_RX;
	writel(value, ioaddr + MAC_CTRL_REG);
	return;
}

/**
 * stmmac_mac_enable_rx
 * @dev: net device structure
 * Description: the function enables the TX MAC process
 */
static void stmmac_mac_enable_tx(struct net_device *dev)
{
	unsigned long ioaddr = dev->base_addr;
	u32 value = readl(ioaddr + MAC_CTRL_REG);

	/* set: TE (transmitter enable, bit 3) */
	value |= MAC_ENABLE_TX;
	writel(value, ioaddr + MAC_CTRL_REG);
	return;
}

/**
 * stmmac_mac_disable_rx
 * @dev: net device structure
 * Description: the function disables the RX MAC process
 */
static void stmmac_mac_disable_rx(struct net_device *dev)
{
	unsigned long ioaddr = dev->base_addr;
	u32 value = readl(ioaddr + MAC_CTRL_REG);

	value &= ~MAC_RNABLE_RX;
	writel(value, ioaddr + MAC_CTRL_REG);
	return;
}

/**
 * stmmac_mac_disable_tx
 * @dev: net device structure
 * Description: the function disables the TX MAC process
 */
static void stmmac_mac_disable_tx(struct net_device *dev)
{
	unsigned long ioaddr = dev->base_addr;
	u32 value = readl(ioaddr + MAC_CTRL_REG);

	value &= ~MAC_ENABLE_TX;
	writel(value, ioaddr + MAC_CTRL_REG);
	return;
}

static void display_ring(struct dma_desc *p, int size)
{
	struct tmp_s {
		u64 a;
		unsigned int b;
		unsigned int c;
	};
	int i;
	for (i = 0; i < size; i++) {
		struct tmp_s *x = (struct tmp_s *)(p + i);
		pr_info("\t%d [0x%x]: DES0=0x%x DES1=0x%x BUF1=0x%x BUF2=0x%x",
		       i, (unsigned int)virt_to_phys(&p[i]),
		       (unsigned int)(x->a), (unsigned int)((x->a) >> 32),
		       x->b, x->c);
		pr_info("\n");
	}
}

/**
 * init_dma_desc_rings - init the RX/TX descriptor rings
 * @dev: net device structure
 * Description:  this function initializes the DMA RX/TX descriptors
 */
static void init_dma_desc_rings(struct net_device *dev)
{
	int i;
	struct stmmac_priv *priv = netdev_priv(dev);
	struct sk_buff *skb;
	unsigned int txsize = priv->dma_tx_size;
	unsigned int rxsize = priv->dma_rx_size;
	unsigned int bfsize = priv->dma_buf_sz;
	int buff2_needed = 0;

	/* Set the Buffer size according to the MTU. */
	if (unlikely(dev->mtu >= BUF_SIZE_8KiB))
		bfsize = BUF_SIZE_16KiB;
	else if (unlikely(dev->mtu >= BUF_SIZE_4KiB))
		bfsize = BUF_SIZE_8KiB;
	else if (unlikely(dev->mtu >= BUF_SIZE_2KiB))
		bfsize = BUF_SIZE_4KiB;
	else if (unlikely(dev->mtu >= DMA_BUFFER_SIZE))
		bfsize = BUF_SIZE_2KiB;
	else
		bfsize = DMA_BUFFER_SIZE;

	/* If the MTU exceeds 8k so use the second buffer in the chain */
	if (bfsize >= BUF_SIZE_8KiB)
		buff2_needed = 1;

	DBG(probe, INFO, "stmmac: txsize %d, rxsize %d, bfsize %d\n",
	    txsize, rxsize, bfsize);

	priv->rx_skbuff_dma = kmalloc(rxsize * sizeof(dma_addr_t), GFP_KERNEL);
	priv->rx_skbuff =
	    kmalloc(sizeof(struct sk_buff *) * rxsize, GFP_KERNEL);
	priv->dma_rx =
	    (struct dma_desc *)dma_alloc_coherent(priv->device,
						  rxsize *
						  sizeof(struct dma_desc),
						  &priv->dma_rx_phy,
						  GFP_KERNEL);
	priv->tx_skbuff = kmalloc(sizeof(struct sk_buff *) * txsize,
				       GFP_KERNEL);
	priv->dma_tx =
	    (struct dma_desc *)dma_alloc_coherent(priv->device,
						  txsize *
						  sizeof(struct dma_desc),
						  &priv->dma_tx_phy,
						  GFP_KERNEL);

	if ((priv->dma_rx == NULL) || (priv->dma_tx == NULL)) {
		pr_err("%s:ERROR allocating the DMA Tx/Rx desc\n", __func__);
		return;
	}

	DBG(probe, INFO, "stmmac (%s) DMA desc rings: virt addr (Rx %p, "
	    "Tx %p)\n\tDMA phy addr (Rx 0x%08x, Tx 0x%08x)\n",
	    dev->name, priv->dma_rx, priv->dma_tx,
	    (unsigned int)priv->dma_rx_phy, (unsigned int)priv->dma_tx_phy);

	/* RX INITIALIZATION */
	DBG(probe, INFO, "stmmac: SKB addresses:\n"
			 "skb\t\tskb data\tdma data\n");

	for (i = 0; i < rxsize; i++) {
		struct dma_desc *p = priv->dma_rx + i;

		skb = netdev_alloc_skb(dev, bfsize);
		if (unlikely(skb == NULL)) {
			pr_err("%s: Rx init fails; skb is NULL\n",
			       __func__);
			break;
		}
		skb_reserve(skb, STMMAC_IP_ALIGN);

		priv->rx_skbuff[i] = skb;
		priv->rx_skbuff_dma[i] = dma_map_single(priv->device,
						skb->data,
						bfsize - STMMAC_IP_ALIGN,
						DMA_FROM_DEVICE);
		p->des2 = priv->rx_skbuff_dma[i];
		if (unlikely(buff2_needed))
			p->des3 = p->des2 + BUF_SIZE_8KiB;
		DBG(probe, INFO, "[%p]\t[%p]\t[%x]\n", priv->rx_skbuff[i],
			priv->rx_skbuff[i]->data, priv->rx_skbuff_dma[i]);
	}
	priv->cur_rx = 0;
	priv->dirty_rx = (unsigned int)(i - rxsize);
	priv->dma_buf_sz = bfsize;
	buf_sz = bfsize;

	/* TX INITIALIZATION */
	for (i = 0; i < txsize; i++) {
		priv->tx_skbuff[i] = NULL;
		priv->dma_tx[i].des2 = 0;
	}
	priv->dirty_tx = 0;
	priv->cur_tx = 0;

	/* Clear the Rx/Tx descriptors */
	priv->mac_type->ops->init_rx_desc(priv->dma_rx, rxsize);
	priv->mac_type->ops->disable_rx_ic(priv->dma_rx, rxsize, rx_coalesce);
	priv->mac_type->ops->init_tx_desc(priv->dma_tx, txsize);

	if (netif_msg_hw(priv)) {
		pr_info("RX descriptor ring:\n");
		display_ring(priv->dma_rx, rxsize);
		pr_info("TX descriptor ring:\n");
		display_ring(priv->dma_tx, txsize);
	}
	return;
}

/**
 * dma_free_rx_skbufs
 * @dev: net device structure
 * Description:  this function frees all the skbuffs in the Rx queue
 */
static void dma_free_rx_skbufs(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	int i;

	for (i = 0; i < priv->dma_rx_size; i++) {
		if (priv->rx_skbuff[i]) {
			dma_unmap_single(priv->device, priv->rx_skbuff_dma[i],
					 priv->dma_buf_sz, DMA_FROM_DEVICE);
			dev_kfree_skb(priv->rx_skbuff[i]);
		}
		priv->rx_skbuff[i] = NULL;
	}
	return;
}

/**
 * dma_free_tx_skbufs
 * @dev: net device structure
 * Description:  this function frees all the skbuffs in the Tx queue
 */
static void dma_free_tx_skbufs(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	int i;

	for (i = 0; i < priv->dma_tx_size; i++) {
		if (priv->tx_skbuff[i] != NULL) {
			struct dma_desc *p = priv->dma_tx + i;
			if (p->des2)
				dma_unmap_single(priv->device, p->des2,
				 priv->mac_type->ops->get_tx_len(p),
				 DMA_TO_DEVICE);
			dev_kfree_skb_any(priv->tx_skbuff[i]);
			priv->tx_skbuff[i] = NULL;
		}
	}
	return;
}

/**
 * free_dma_desc_resources
 * @dev: net device structure
 * Description:  this function releases and free ALL the DMA resources
 */
static void free_dma_desc_resources(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);

	/* Release the DMA TX/RX socket buffers */
	dma_free_rx_skbufs(dev);
	dma_free_tx_skbufs(dev);

	/* Free the region of consistent memory previously allocated for
	 * the DMA */
	dma_free_coherent(priv->device,
			  priv->dma_tx_size * sizeof(struct dma_desc),
			  priv->dma_tx, priv->dma_tx_phy);
	dma_free_coherent(priv->device,
			  priv->dma_rx_size * sizeof(struct dma_desc),
			  priv->dma_rx, priv->dma_rx_phy);
	kfree(priv->rx_skbuff_dma);
	kfree(priv->rx_skbuff);
	kfree(priv->tx_skbuff);

	return;
}

/**
 * stmmac_dma_start_tx
 * @ioaddr: device I/O address
 * Description:  this function starts the DMA tx process
 */
static void stmmac_dma_start_tx(unsigned long ioaddr)
{
	u32 value = readl(ioaddr + DMA_CONTROL);
	value |= DMA_CONTROL_ST;
	writel(value, ioaddr + DMA_CONTROL);
	return;
}

static void stmmac_dma_stop_tx(unsigned long ioaddr)
{
	u32 value = readl(ioaddr + DMA_CONTROL);
	value &= ~DMA_CONTROL_ST;
	writel(value, ioaddr + DMA_CONTROL);
	return;
}

/**
 * stmmac_dma_start_rx
 * @ioaddr: device I/O address
 * Description:  this function starts the DMA rx process
 */
static void stmmac_dma_start_rx(unsigned long ioaddr)
{
	u32 value = readl(ioaddr + DMA_CONTROL);
	value |= DMA_CONTROL_SR;
	writel(value, ioaddr + DMA_CONTROL);

	return;
}

static void stmmac_dma_stop_rx(unsigned long ioaddr)
{
	u32 value = readl(ioaddr + DMA_CONTROL);
	value &= ~DMA_CONTROL_SR;
	writel(value, ioaddr + DMA_CONTROL);

	return;
}

/**
 *  stmmac_dma_operation_mode - HW DMA operation mode
 *  @dev : pointer to the device structure.
 *  Description:
 *  This function sets the DMA operation mode: tx/rx DMA thresholds
 *  or Store-And-Forward capability. It also verifies the COE for the
 *  transmission in case of Giga ETH.
 */
static void stmmac_dma_operation_mode(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);

	if (!priv->is_gmac) {
		/* MAC 10/100 */
		priv->mac_type->ops->dma_mode(dev->base_addr,
					      priv->xstats.threshold, 0);
		priv->tx_coe = NO_HW_CSUM;
	} else {
		if ((dev->mtu <= ETH_DATA_LEN) && (tx_coe)) {
			priv->mac_type->ops->dma_mode(dev->base_addr,
						      SF_DMA_MODE, SF_DMA_MODE);
			priv->tx_coe = HW_CSUM;
		} else {
			/* Checksum computation is performed in software. */
			priv->mac_type->ops->dma_mode(dev->base_addr,
						      priv->xstats.threshold,
						      SF_DMA_MODE);
			priv->tx_coe = NO_HW_CSUM;
		}
	}
	tx_coe = priv->tx_coe;

	return;
}

static inline void stmmac_dma_enable_irq_rx(unsigned long ioaddr)
{
	writel(DMA_INTR_DEFAULT_MASK, ioaddr + DMA_INTR_ENA);
	return;
}

static inline void stmmac_dma_disable_irq_rx(unsigned long ioaddr)
{
	writel(DMA_INTR_NO_RX, ioaddr + DMA_INTR_ENA);
	return;
}

#ifdef STMMAC_DEBUG
/**
 * show_tx_process_state
 * @status: tx descriptor status field
 * Description: it shows the Transmit Process State for CSR5[22:20]
 */
static void show_tx_process_state(unsigned int status)
{
	unsigned int state;
	state = (status & DMA_STATUS_TS_MASK) >> DMA_STATUS_TS_SHIFT;

	switch (state) {
	case 0:
		pr_info("- TX (Stopped): Reset or Stop command\n");
		break;
	case 1:
		pr_info("- TX (Running):Fetching the Tx desc\n");
		break;
	case 2:
		pr_info("- TX (Running): Waiting for end of tx\n");
		break;
	case 3:
		pr_info("- TX (Running): Reading the data "
		       "and queuing the data into the Tx buf\n");
		break;
	case 6:
		pr_info("- TX (Suspended): Tx Buff Underflow "
		       "or an unavailable Transmit descriptor\n");
		break;
	case 7:
		pr_info("- TX (Running): Closing Tx descriptor\n");
		break;
	default:
		break;
	}
	return;
}

/**
 * show_rx_process_state
 * @status: rx descriptor status field
 * Description: it shows the  Receive Process State for CSR5[19:17]
 */
static void show_rx_process_state(unsigned int status)
{
	unsigned int state;
	state = (status & DMA_STATUS_RS_MASK) >> DMA_STATUS_RS_SHIFT;

	switch (state) {
	case 0:
		pr_info("- RX (Stopped): Reset or Stop command\n");
		break;
	case 1:
		pr_info("- RX (Running): Fetching the Rx desc\n");
		break;
	case 2:
		pr_info("- RX (Running):Checking for end of pkt\n");
		break;
	case 3:
		pr_info("- RX (Running): Waiting for Rx pkt\n");
		break;
	case 4:
		pr_info("- RX (Suspended): Unavailable Rx buf\n");
		break;
	case 5:
		pr_info("- RX (Running): Closing Rx descriptor\n");
		break;
	case 6:
		pr_info("- RX(Running): Flushing the current frame"
		       " from the Rx buf\n");
		break;
	case 7:
		pr_info("- RX (Running): Queuing the Rx frame"
		       " from the Rx buf into memory\n");
		break;
	default:
		break;
	}
	return;
}
#endif

/**
 * stmmac_tx:
 * @dev: net device structure
 * Description: reclaim resources after transmit completes.
 */
static void stmmac_tx(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	unsigned int txsize = priv->dma_tx_size;
	unsigned long ioaddr = dev->base_addr;
	unsigned int entry = priv->dirty_tx % txsize;

	spin_lock(&priv->tx_lock);
	while (priv->dirty_tx != priv->cur_tx) {
		int last;
		struct dma_desc *p = priv->dma_tx + entry;

		if (priv->mac_type->ops->get_tx_owner(p))
			break;

		/* verify tx error by looking at the last segment */
		last = priv->mac_type->ops->get_tx_ls(p);
		if (likely(last)) {
			int tx_error =
			    priv->mac_type->ops->tx_status(&dev->stats,
							   &priv->xstats,
							   p, ioaddr);
			if (likely(tx_error == 0)) {
				dev->stats.tx_packets++;
				priv->xstats.tx_pkt_n++;
			} else
				dev->stats.tx_errors++;
		}
		DBG(intr, DEBUG, "stmmac_tx: curr %d, dirty %d\n",
		    priv->cur_tx, priv->dirty_tx);

		if (likely(p->des2))
			dma_unmap_single(priv->device, p->des2,
					 priv->mac_type->ops->get_tx_len(p),
					 DMA_TO_DEVICE);
		if (unlikely(p->des3))
			p->des3 = 0;

		if (likely(priv->tx_skbuff[entry] != NULL)) {
			dev_kfree_skb_irq(priv->tx_skbuff[entry]);
			priv->tx_skbuff[entry] = NULL;
		}

		priv->mac_type->ops->release_tx_desc(p);

		entry = (++priv->dirty_tx) % txsize;
	}
	if (unlikely(netif_queue_stopped(dev) &&
		     stmmac_tx_avail(priv) > (MAX_SKB_FRAGS + 1)))
		netif_wake_queue(dev);

	spin_unlock(&priv->tx_lock);
	return;
}

/**
 * stmmac_schedule_rx:
 * @dev: net device structure
 * Description: it schedules the reception process.
 */
static void stmmac_schedule_rx(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	stmmac_dma_disable_irq_rx(dev->base_addr);

	napi_schedule(&priv->napi);

	return;
}

static void stmmac_tx_tasklet(unsigned long data)
{
	struct net_device *dev = (struct net_device *)data;
	struct stmmac_priv *priv = netdev_priv(dev);

	priv->xstats.tx_task_n++;
	stmmac_tx(dev);

#ifdef CONFIG_STMMAC_TIMER
	priv->tm->timer_start(tmrate);
#endif
	return;
}

#ifdef CONFIG_STMMAC_TIMER
void stmmac_timer_work(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);

	unsigned int rxentry = priv->cur_rx % priv->dma_rx_size;
	unsigned int txentry = priv->dirty_tx % priv->dma_tx_size;
	int rxret, txret;

	/* Look at if there is pending work to do; otherwise, do not spend
	   any other time here. */
	rxret = priv->mac_type->ops->get_rx_owner(priv->dma_rx + rxentry);
	if (likely(rxret == 0))
		stmmac_schedule_rx(dev);

	txret = priv->mac_type->ops->get_tx_owner(priv->dma_rx + txentry);
	if (likely(txret == 0))
		tasklet_schedule(&priv->tx_task);

	/* Timer will be re-started later. */
	if (likely(rxret == 0) || (rxret == 0))
		priv->tm->timer_stop();

	return;
}

static void stmmac_no_timer_started(unsigned int x)
{;
};

static void stmmac_no_timer_stopped(void)
{;
};
#endif

/**
 * stmmac_tx_err:
 * @dev: net device structure
 * Description: clean descriptors and restart the transmission.
 */
static void stmmac_tx_err(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);

	spin_lock(&priv->tx_lock);

	netif_stop_queue(dev);

	stmmac_dma_stop_tx(dev->base_addr);
	dma_free_tx_skbufs(dev);
	priv->mac_type->ops->init_tx_desc(priv->dma_tx, priv->dma_tx_size);
	priv->dirty_tx = 0;
	priv->cur_tx = 0;
	stmmac_dma_start_tx(dev->base_addr);

	dev->stats.tx_errors++;
	netif_wake_queue(dev);

	spin_unlock(&priv->tx_lock);
	return;
}

/**
 * stmmac_dma_interrupt - Interrupt handler for the STMMAC DMA
 * @dev: net device structure
 * Description: it determines if we have to call either the Rx or the Tx
 * interrupt handler.
 */
static void stmmac_dma_interrupt(struct net_device *dev)
{
	unsigned long ioaddr = dev->base_addr;
	struct stmmac_priv *priv = netdev_priv(dev);
	/* read the status register (CSR5) */
	u32 intr_status = readl(ioaddr + DMA_STATUS);

	DBG(intr, INFO, "%s: [CSR5: 0x%08x]\n", __func__, intr_status);

#ifdef STMMAC_DEBUG
	/* It displays the DMA transmit process state (CSR5 register) */
	if (netif_msg_tx_done(priv))
		show_tx_process_state(intr_status);
	if (netif_msg_rx_status(priv))
		show_rx_process_state(intr_status);
#endif
	/* Clear the interrupt by writing a logic 1 to the CSR5[15-0] */
	writel((intr_status & 0x1ffff), ioaddr + DMA_STATUS);

	/* ABNORMAL interrupts */
	if (unlikely(intr_status & DMA_STATUS_AIS)) {
		DBG(intr, INFO, "CSR5[15] DMA ABNORMAL IRQ: ");
		if (unlikely(intr_status & DMA_STATUS_UNF)) {
			DBG(intr, INFO, "transmit underflow\n");
			if ((priv->xstats.threshold != SF_DMA_MODE)
			    && (priv->xstats.threshold <= 256)) {
				/* Try to bump up the threshold */
				priv->xstats.threshold += 64;
				priv->mac_type->ops->dma_mode(ioaddr,
					      priv->xstats.threshold,
					      SF_DMA_MODE);
			}
			stmmac_tx_err(dev);
			priv->xstats.tx_undeflow_irq++;
		}
		if (unlikely(intr_status & DMA_STATUS_TJT)) {
			DBG(intr, INFO, "transmit jabber\n");
			priv->xstats.tx_jabber_irq++;
		}
		if (unlikely(intr_status & DMA_STATUS_OVF)) {
			DBG(intr, INFO, "recv overflow\n");
			priv->xstats.rx_overflow_irq++;
		}
		if (unlikely(intr_status & DMA_STATUS_RU)) {
			DBG(intr, INFO, "receive buffer unavailable\n");
			priv->xstats.rx_buf_unav_irq++;
		}
		if (unlikely(intr_status & DMA_STATUS_RPS)) {
			DBG(intr, INFO, "receive process stopped\n");
			priv->xstats.rx_process_stopped_irq++;
		}
		if (unlikely(intr_status & DMA_STATUS_RWT)) {
			DBG(intr, INFO, "receive watchdog\n");
			priv->xstats.rx_watchdog_irq++;
		}
		if (unlikely(intr_status & DMA_STATUS_ETI)) {
			DBG(intr, INFO, "transmit early interrupt\n");
			priv->xstats.tx_early_irq++;
		}
		if (unlikely(intr_status & DMA_STATUS_TPS)) {
			DBG(intr, INFO, "transmit process stopped\n");
			priv->xstats.tx_process_stopped_irq++;
			stmmac_tx_err(dev);
		}
		if (unlikely(intr_status & DMA_STATUS_FBI)) {
			DBG(intr, INFO, "fatal bus error\n");
			priv->xstats.fatal_bus_error_irq++;
			stmmac_tx_err(dev);
		}
	}

	/* NORMAL interrupts */
	if (intr_status & DMA_STATUS_NIS) {
		DBG(intr, INFO, " CSR5[16]: DMA NORMAL IRQ: ");
		if (intr_status & DMA_STATUS_RI) {

			RX_DBG("Receive irq [buf: 0x%08x]\n",
			       readl(ioaddr + DMA_CUR_RX_BUF_ADDR));
			priv->xstats.dma_rx_normal_irq++;
			stmmac_schedule_rx(dev);
		}
		if (intr_status & (DMA_STATUS_TI)) {
			DBG(intr, INFO, " Transmit irq [buf: 0x%08x]\n",
			    readl(ioaddr + DMA_CUR_TX_BUF_ADDR));
			priv->xstats.dma_tx_normal_irq++;
			tasklet_schedule(&priv->tx_task);
		}
	}

	/* Optional hardware blocks, interrupts should be disabled */
	if (unlikely(intr_status &
		     (DMA_STATUS_GPI | DMA_STATUS_GMI | DMA_STATUS_GLI)))
		pr_info("%s: unexpected status %08x\n", __func__, intr_status);

	DBG(intr, INFO, "\n\n");

	return;
}

/**
 *  stmmac_open - open entry point of the driver
 *  @dev : pointer to the device structure.
 *  Description:
 *  This function is the open entry point of the driver.
 *  Return value:
 *  0 on success and an appropriate (-)ve integer as defined in errno.h
 *  file on failure.
 */
static int stmmac_open(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;
	int ret;

	/* Check that the MAC address is valid.  If its not, refuse
	 * to bring the device up. The user must specify an
	 * address using the following linux command:
	 *      ifconfig eth0 hw ether xx:xx:xx:xx:xx:xx  */
	if (!is_valid_ether_addr(dev->dev_addr)) {
		random_ether_addr(dev->dev_addr);
		pr_warning("%s: generated random MAC address "
			"%.2x:%.2x:%.2x:%.2x:%.2x:%.2x.\n", dev->name,
			dev->dev_addr[0], dev->dev_addr[1],
			dev->dev_addr[2], dev->dev_addr[3],
			dev->dev_addr[4], dev->dev_addr[5]);
	}

	stmmac_verify_args();

	ret = stmmac_init_phy(dev);
	if (unlikely(ret)) {
		pr_err("%s: Cannot attach to PHY (error: %d)\n", __func__, ret);
		return ret;
	}

	/* Request the IRQ lines */
	ret = request_irq(dev->irq, &stmmac_interrupt,
			  IRQF_SHARED, dev->name, dev);
	if (unlikely(ret < 0)) {
		printk(KERN_ERR
		       "%s: ERROR: allocating the IRQ %d (error: %d)\n",
		       __func__, dev->irq, ret);
		return ret;
	}

#ifdef CONFIG_STMMAC_TIMER
	priv->tm = kmalloc(sizeof(struct stmmac_timer *), GFP_KERNEL);
	if (unlikely(priv->tm == NULL)) {
		pr_err("%s: ERROR: timer memory alloc failed \n", __func__);
		return -ENOMEM;
	}
	priv->tm->freq = tmrate;

	/* Test if the HW timer can be actually used.
	 * In case of failure go haead without using any timers. */
	if (unlikely((stmmac_open_hw_timer(dev, priv->tm)) < 0)) {
		pr_warning("stmmaceth: cannot attach the HW timer\n");
		rx_coalesce = 1;
		tmrate = 0;
		priv->tm->freq = 0;
		priv->tm->timer_start = stmmac_no_timer_started;
		priv->tm->timer_stop = stmmac_no_timer_stopped;
	}
#endif

	/* Create and initialize the TX/RX descriptors chains */
	priv->dma_tx_size = STMMAC_ALIGN(dma_txsize);
	priv->dma_rx_size = STMMAC_ALIGN(dma_rxsize);
	priv->dma_buf_sz = STMMAC_ALIGN(buf_sz);
	init_dma_desc_rings(dev);

	/* DMA initialization and SW reset */
	if (unlikely(priv->mac_type->ops->dma_init(ioaddr,
		priv->pbl, priv->dma_tx_phy, priv->dma_rx_phy) < 0)) {

		pr_err("%s: DMA initialization failed\n", __func__);
		return -1;
	}

	/* Copy the MAC addr into the HW (in case we have set it with nwhw) */
	priv->mac_type->ops->set_umac_addr(ioaddr, dev->dev_addr, 0);

	/* Initialize the MAC Core */
	priv->mac_type->ops->core_init(ioaddr);

	priv->tx_coalesce = 0;
	priv->shutdown = 0;

	/* Initialise the MMC (if present) to disable all interrupts */
	writel(0xffffffff, ioaddr + MMC_HIGH_INTR_MASK);
	writel(0xffffffff, ioaddr + MMC_LOW_INTR_MASK);

	/* Enable the MAC Rx/Tx */
	stmmac_mac_enable_rx(dev);
	stmmac_mac_enable_tx(dev);

	/* Extra statistics */
	memset(&priv->xstats, 0, sizeof(struct stmmac_extra_stats));
	priv->xstats.threshold = tc;

	/* Set the HW DMA mode and the COE */
	stmmac_dma_operation_mode(dev);

	/* Start the ball rolling... */
	DBG(probe, DEBUG, "%s: DMA RX/TX processes started...\n", dev->name);
	stmmac_dma_start_tx(ioaddr);
	stmmac_dma_start_rx(ioaddr);

#ifdef CONFIG_STMMAC_TIMER
	priv->tm->timer_start(tmrate);
#endif
	tasklet_init(&priv->tx_task, stmmac_tx_tasklet, (unsigned long)dev);

	/* Dump DMA/MAC registers */
	if (netif_msg_hw(priv)) {
		priv->mac_type->ops->dump_mac_regs(ioaddr);
		priv->mac_type->ops->dump_dma_regs(ioaddr);
	}

	if (priv->phydev)
		phy_start(priv->phydev);

	napi_enable(&priv->napi);
#if 0
	/* FIXME: Owing to some hw issues met on COE; it is safe to control
	 * (and limit) the size of the TSO frames. */
	if (priv->is_gmac)
		netif_set_gso_max_size(dev, BUF_SIZE_4KiB);
#endif
	netif_start_queue(dev);
	return 0;
}

/**
 *  stmmac_release - close entry point of the driver
 *  @dev : device pointer.
 *  Description:
 *  This is the stop entry point of the driver.
 */
static int stmmac_release(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);

	/* Stop and disconnect the PHY */
	if (priv->phydev) {
		phy_stop(priv->phydev);
		phy_disconnect(priv->phydev);
		priv->phydev = NULL;
	}

	netif_stop_queue(dev);
	tasklet_kill(&priv->tx_task);

#ifdef CONFIG_STMMAC_TIMER
	/* Stop and release the timer */
	stmmac_close_hw_timer();
	if (priv->tm != NULL)
		kfree(priv->tm);
#endif
	napi_disable(&priv->napi);

	/* Free the IRQ lines */
	free_irq(dev->irq, dev);

	/* Stop TX/RX DMA and clear the descriptors */
	stmmac_dma_stop_tx(dev->base_addr);
	stmmac_dma_stop_rx(dev->base_addr);

	/* Release and free the Rx/Tx resources */
	free_dma_desc_resources(dev);

	/* Disable the MAC core */
	stmmac_mac_disable_tx(dev);
	stmmac_mac_disable_rx(dev);

	netif_carrier_off(dev);

	return 0;
}

/*
 * To perform emulated hardware segmentation on skb.
 */
static int stmmac_sw_tso(struct stmmac_priv *priv, struct sk_buff *skb)
{
	struct sk_buff *segs, *curr_skb;
	int gso_segs = skb_shinfo(skb)->gso_segs;

	/* Estimate the number of fragments in the worst case */
	if (unlikely(stmmac_tx_avail(priv) <= gso_segs * 3)) {
		netif_stop_queue(priv->dev);
		pr_err("%s: TSO BUG! Tx Ring full when queue awake\n",
		       __func__);
		return NETDEV_TX_BUSY;
	}
#ifdef STMMAC_XMIT_DEBUG
	pr_debug("\tstmmac_sw_tso: segmenting: skb %p (len %d)\n",
	       skb, skb->len);
#endif

	segs = skb_gso_segment(skb, priv->dev->features & ~NETIF_F_TSO);
	if (unlikely(IS_ERR(segs)))
		goto drop;

	do {
		curr_skb = segs;
		segs = segs->next;
#ifdef STMMAC_XMIT_DEBUG
		pr_debug("\t\tcurrent skb->len: %d, *curr %p,"
		       "*next %p\n", curr_skb->len, curr_skb, segs);
#endif
		curr_skb->next = NULL;
		stmmac_xmit(curr_skb, priv->dev);
	} while (segs);

	dev_kfree_skb(skb);
	return NETDEV_TX_OK;

drop:
#ifdef STMMAC_XMIT_DEBUG
	pr_debug("\t\tdropped!\n");
#endif
	priv->dev->stats.tx_dropped += 1;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static unsigned int stmmac_handle_jumbo_frames(struct sk_buff *skb,
					       struct net_device *dev,
					       int csum_insertion)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	unsigned int nopaged_len = skb_headlen(skb);
	unsigned int txsize = priv->dma_tx_size;
	unsigned int entry = priv->cur_tx % txsize;
	struct dma_desc *desc = priv->dma_tx + entry;

	if (nopaged_len > BUF_SIZE_8KiB) {

		int buf2_size = nopaged_len - BUF_SIZE_8KiB;

		desc->des2 = dma_map_single(priv->device, skb->data,
					    BUF_SIZE_8KiB, DMA_TO_DEVICE);
		desc->des3 = desc->des2 + BUF_SIZE_4KiB;
		priv->mac_type->ops->prepare_tx_desc(desc, 1, BUF_SIZE_8KiB,
						     csum_insertion);

		entry = (++priv->cur_tx) % txsize;
		desc = priv->dma_tx + entry;

		desc->des2 = dma_map_single(priv->device,
					skb->data + BUF_SIZE_8KiB,
					buf2_size, DMA_TO_DEVICE);
		desc->des3 = desc->des2 + BUF_SIZE_4KiB;
		priv->mac_type->ops->prepare_tx_desc(desc, 0,
						     buf2_size, csum_insertion);
		priv->mac_type->ops->set_tx_owner(desc);
		priv->tx_skbuff[entry] = NULL;
	} else {
		desc->des2 = dma_map_single(priv->device, skb->data,
					nopaged_len, DMA_TO_DEVICE);
		desc->des3 = desc->des2 + BUF_SIZE_4KiB;
		priv->mac_type->ops->prepare_tx_desc(desc, 1, nopaged_len,
						     csum_insertion);
	}
	return entry;
}

/**
 *  stmmac_xmit:
 *  @skb : the socket buffer
 *  @dev : device pointer
 *  Description : Tx entry point of the driver.
 */
static int stmmac_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	unsigned long flags;
	unsigned int txsize = priv->dma_tx_size;
	unsigned int entry;
	int i, ret, csum_insertion = 0, nfrags = skb_shinfo(skb)->nr_frags;
	struct dma_desc *desc, *first;

	if (unlikely(!spin_trylock_irqsave(&priv->tx_lock, flags)))
		/* Collision - tell upper layer to requeue */
		return NETDEV_TX_LOCKED;

	entry = priv->cur_tx % txsize;
	ret = NETDEV_TX_OK;

	/* This is a hard error log it. */
	if (unlikely(stmmac_tx_avail(priv) < nfrags + 1)) {
		netif_stop_queue(dev);
		pr_err("%s: BUG! Tx Ring full when queue awake\n",
		       __func__);
		ret = NETDEV_TX_BUSY;
		goto end_xmit;
	}

	if (unlikely((priv->tx_skbuff[entry] != NULL))) {
		pr_err("%s: BUG! Inconsistent Tx skb utilization\n",
		       __func__);
		dev_kfree_skb_any(skb);
		dev->stats.tx_dropped += 1;
		ret = -1;
		goto end_xmit;
	}
#ifdef STMMAC_XMIT_DEBUG
	if ((skb->len > ETH_FRAME_LEN) || nfrags)
		pr_info("stmmac xmit:\n"
		       "\tskb addr %p - len: %d - nopaged_len: %d\n"
		       "\tn_frags: %d - ip_summed: %d - %s gso\n",
		       skb, skb->len, skb_headlen(skb), nfrags, skb->ip_summed,
		       !skb_is_gso(skb) ? "isn't" : "is");
#endif

	if (skb_is_gso(skb)) {
		ret = stmmac_sw_tso(priv, skb);
		goto end_xmit;
	}

	if (likely((skb->ip_summed == CHECKSUM_PARTIAL))) {
		if (likely(priv->tx_coe == NO_HW_CSUM))
			skb_checksum_help(skb);
		else
			csum_insertion = 1;
	}

	desc = priv->dma_tx + entry;
	first = desc;

#ifdef STMMAC_XMIT_DEBUG
	if ((nfrags > 0) || (skb->len > ETH_FRAME_LEN))
		pr_debug("stmmac xmit: skb len: %d, nopaged_len: %d,\n"
		       "\t\tn_frags: %d, ip_summed: %d\n",
		       skb->len, skb_headlen(skb), nfrags, skb->ip_summed);
#endif
	priv->tx_skbuff[entry] = skb;
	if (unlikely(skb->len >= BUF_SIZE_4KiB)) {
		entry = stmmac_handle_jumbo_frames(skb, dev, csum_insertion);
		desc = priv->dma_tx + entry;
	} else {
		unsigned int nopaged_len = skb_headlen(skb);
		desc->des2 = dma_map_single(priv->device, skb->data,
					nopaged_len, DMA_TO_DEVICE);
		priv->mac_type->ops->prepare_tx_desc(desc, 1, nopaged_len,
						     csum_insertion);
	}

	for (i = 0; i < nfrags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		int len = frag->size;

		entry = (++priv->cur_tx) % txsize;
		desc = priv->dma_tx + entry;

#ifdef STMMAC_XMIT_DEBUG
		pr_info("\t[entry %d] segment len: %d\n", entry, len);
#endif
		desc->des2 = dma_map_page(priv->device, frag->page,
					  frag->page_offset,
					  len, DMA_TO_DEVICE);
		priv->tx_skbuff[entry] = NULL;
		priv->mac_type->ops->prepare_tx_desc(desc, 0, len,
						     csum_insertion);
		priv->mac_type->ops->set_tx_owner(desc);
	}

	/* Interrupt on completition only for the latest segment */
	priv->mac_type->ops->close_tx_desc(desc);
	/* to avoid raise condition */
	priv->mac_type->ops->set_tx_owner(first);

	priv->cur_tx++;

#ifdef STMMAC_XMIT_DEBUG
	if (netif_msg_pktdata(priv)) {
		pr_info("stmmac xmit: current=%d, dirty=%d, entry=%d, "
		       "first=%p, nfrags=%d\n",
		       (priv->cur_tx % txsize), (priv->dirty_tx % txsize),
		       entry, first, nfrags);
		display_ring(priv->dma_tx, txsize);
		pr_info(">>> frame to be transmitted: ");
		print_pkt(skb->data, skb->len);
	}
#endif
	if (stmmac_tx_avail(priv) <= (MAX_SKB_FRAGS + 1) ||
	    (!(priv->mac_type->hw.link.duplex) && csum_insertion)) {
		netif_stop_queue(dev);
	} else {
		/* Tx interrupts moderation */
		if (priv->tx_coalesce <= tx_coalesce) {
			priv->tx_coalesce++;
			priv->mac_type->ops->clear_tx_ic(desc);
		} else {
			priv->tx_coalesce = 0;
		}
	}

	dev->stats.tx_bytes += skb->len;

	/* CSR1 enables the transmit DMA to check for new descriptor */
	writel(1, dev->base_addr + DMA_XMT_POLL_DEMAND);

	dev->trans_start = jiffies;

end_xmit:
	spin_unlock_irqrestore(&priv->tx_lock, flags);
	return ret;
}

static inline void stmmac_rx_refill(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	unsigned int rxsize = priv->dma_rx_size;
	int bfsize = priv->dma_buf_sz;
	struct dma_desc *p = priv->dma_rx;

	for (; priv->cur_rx - priv->dirty_rx > 0; priv->dirty_rx++) {
		unsigned int entry = priv->dirty_rx % rxsize;
		if (likely(priv->rx_skbuff[entry] == NULL)) {
			struct sk_buff *skb = netdev_alloc_skb(dev, bfsize);
			if (unlikely(skb == NULL)) {
				pr_err("%s: skb is NULL\n", __func__);
				break;
			}
			skb_reserve(skb, STMMAC_IP_ALIGN);
			priv->rx_skbuff[entry] = skb;
			priv->rx_skbuff_dma[entry] =
			    dma_map_single(priv->device, skb->data,
				bfsize - STMMAC_IP_ALIGN, DMA_FROM_DEVICE);
			(p + entry)->des2 = priv->rx_skbuff_dma[entry];
			if (priv->is_gmac) {
				if (bfsize >= BUF_SIZE_8KiB)
					(p + entry)->des3 =
					    (p + entry)->des2 + BUF_SIZE_8KiB;
			}
			RX_DBG("\trefill entry #%d\n", entry);
		}
		priv->mac_type->ops->set_rx_owner(p + entry);
	}
	return;
}

static int stmmac_rx(struct net_device *dev, int limit)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	unsigned int rxsize = priv->dma_rx_size;
	unsigned int entry = priv->cur_rx % rxsize;
	unsigned int next_entry;
	unsigned int count = 0;
	struct dma_desc *p = priv->dma_rx + entry;
	struct dma_desc *p_next;

#ifdef STMMAC_RX_DEBUG
	if (netif_msg_hw(priv)) {
		pr_debug(">>> stmmac_rx: descriptor ring:\n");
		display_ring(priv->dma_rx, rxsize);
	}
#endif
	count = 0;
	prefetch(p);
	while (!priv->mac_type->ops->get_rx_owner(p)) {
		int status;

		if (count >= limit)
			break;

		count++;

		next_entry = (++priv->cur_rx) % rxsize;
		p_next = priv->dma_rx + next_entry;
		prefetch(p_next);

		/* read the status of the incoming frame */
		status = (priv->mac_type->ops->rx_status(&dev->stats,
							 &priv->xstats, p));
		if (unlikely(status == discard_frame))
			dev->stats.rx_errors++;
		else {
			struct sk_buff *skb;
			/* Length should omit the CRC */
			int frame_len =
			    priv->mac_type->ops->get_rx_frame_len(p) - 4;

#ifdef STMMAC_RX_DEBUG
			if (frame_len > ETH_FRAME_LEN)
				pr_debug("\tRX frame size %d, COE status: %d\n",
					frame_len, status);

			if (netif_msg_hw(priv))
				pr_debug("\tdesc: %p [entry %d] buff=0x%x\n",
					p, entry, p->des2);
#endif
			skb = priv->rx_skbuff[entry];
			if (unlikely(!skb)) {
				pr_err("%s: Inconsistent Rx descriptor chain\n",
					dev->name);
				dev->stats.rx_dropped++;
				break;
			}
			prefetch(skb->data - NET_IP_ALIGN);
			priv->rx_skbuff[entry] = NULL;

			skb_put(skb, frame_len);
			dma_unmap_single(priv->device,
					 priv->rx_skbuff_dma[entry],
					 priv->dma_buf_sz, DMA_FROM_DEVICE);
#ifdef STMMAC_RX_DEBUG
			if (netif_msg_pktdata(priv)) {
				pr_info(" frame received (%dbytes)", frame_len);
				print_pkt(skb->data, frame_len);
			}
#endif
			skb->protocol = eth_type_trans(skb, dev);
			if (status == csum_none)
				skb->ip_summed = CHECKSUM_NONE;
			else
				skb->ip_summed = CHECKSUM_UNNECESSARY;

#ifdef STMMAC_VLAN_TAG_USED
			if ((priv->vlgrp != NULL) && (priv->is_gmac) &&
			    (p->des01.erx.vlan_tag)) {
				RX_DBG(KERN_INFO "GMAC RX: VLAN frame tagged"
				       " by the core\n");
				priv->xstats.rx_vlan++;
			}
			/*FIXME*/
#endif
			    netif_receive_skb(skb);

			dev->stats.rx_packets++;
			dev->stats.rx_bytes += frame_len;
			dev->last_rx = jiffies;
		}
		entry = next_entry;
		p = p_next;	/* use prefetched values */
	}

	stmmac_rx_refill(dev);

	return count;
}

/**
 *  stmmac_poll - stmmac poll method (NAPI)
 *  @napi : pointer to the napi structure.
 *  @budget : maximum number of packets that the current CPU can receive from
 *	      all interfaces.
 *  Description :
 *   This function implements the the reception process.
 *   It is based on NAPI which provides a "inherent mitigation" in order
 *   to improve network performance.
 */
static int stmmac_poll(struct napi_struct *napi, int budget)
{
	int work_done;
	struct stmmac_priv *priv = container_of(napi, struct stmmac_priv, napi);
	struct net_device *dev = priv->dev;

	work_done = stmmac_rx(dev, budget);

	/* Update rx internal stats */
	priv->xstats.rx_poll_n++;
	priv->xstats.rx_pkt_n += work_done;

	if (work_done < budget) {
		RX_DBG(">>> rx work completed.\n");
		napi_complete(napi);
		stmmac_dma_enable_irq_rx(dev->base_addr);
#ifdef CONFIG_STMMAC_TIMER
		priv->tm->timer_start(tmrate);
#endif
	}
	return work_done;

}

/**
 *  stmmac_tx_timeout
 *  @dev : Pointer to net device structure
 *  Description: this function is called when a packet transmission fails to
 *   complete within a reasonable tmrate. The driver will mark the error in the
 *   netdev structure and arrange for the device to be reset to a sane state
 *   in order to transmit a new packet.
 */
static void stmmac_tx_timeout(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);

	pr_warning("%s: Tx timeout at %ld, latency %ld\n",
	       dev->name, jiffies, (jiffies - dev->trans_start));

#ifdef STMMAC_DEBUG
	pr_info("(current=%d, dirty=%d)\n", (priv->cur_tx % priv->dma_tx_size),
	       (priv->dirty_tx % priv->dma_tx_size));
	pr_info("DMA tx ring status: \n");
	display_ring(priv->dma_tx, priv->dma_tx_size);
#endif
	/* Remove tx moderation */
	tx_coalesce = -1;
	priv->tx_coalesce = 0;

	/* Clear Tx resources and restart transmitting again */
	stmmac_tx_err(dev);

	dev->trans_start = jiffies;

	return;
}

/* Configuration changes (passed on by ifconfig) */
static int stmmac_config(struct net_device *dev, struct ifmap *map)
{
	if (dev->flags & IFF_UP)	/* can't act on a running interface */
		return -EBUSY;

	/* Don't allow changing the I/O address */
	if (map->base_addr != dev->base_addr) {
		pr_warning("%s: can't change I/O address\n", dev->name);
		return -EOPNOTSUPP;
	}

	/* Don't allow changing the IRQ */
	if (map->irq != dev->irq) {
		pr_warning("%s: can't change IRQ number %d\n",
		       dev->name, dev->irq);
		return -EOPNOTSUPP;
	}

	/* ignore other fields */
	return 0;
}

/**
 *  stmmac_multicast_list - entry point for multicast addressing
 *  @dev : pointer to the device structure
 *  Description:
 *  This function is a driver entry point which gets called by the kernel
 *  whenever multicast addresses must be enabled/disabled.
 *  Return value:
 *  void.
 */
static void stmmac_multicast_list(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);

	spin_lock(&priv->lock);
	priv->mac_type->ops->set_filter(dev);
	spin_unlock(&priv->lock);
	return;
}

/**
 *  stmmac_change_mtu - entry point to change MTU size for the device.
 *   @dev : device pointer.
 *   @new_mtu : the new MTU size for the device.
 *   Description: the Maximum Transfer Unit (MTU) is used by the network layer
 *     to drive packet transmission. Ethernet has an MTU of 1500 octets
 *     (ETH_DATA_LEN). This value can be changed with ifconfig.
 *  Return value:
 *   0 on success and an appropriate (-)ve integer as defined in errno.h
 *   file on failure.
 */
static int stmmac_change_mtu(struct net_device *dev, int new_mtu)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	int max_mtu;

	if (netif_running(dev)) {
		printk(KERN_ERR
		       "%s: must be stopped to change its MTU\n", dev->name);
		return -EBUSY;
	}

	if (priv->is_gmac)
		max_mtu = JUMBO_LEN;
	else
		max_mtu = ETH_DATA_LEN;

	if ((new_mtu < 46) || (new_mtu > max_mtu)) {
		printk(KERN_ERR
		       "%s: invalid MTU, max MTU is: %d\n", dev->name, max_mtu);
		return -EINVAL;
	}

	dev->mtu = new_mtu;

	return 0;
}

static irqreturn_t stmmac_interrupt(int irq, void *dev_id)
{
	struct net_device *dev = (struct net_device *)dev_id;
	struct stmmac_priv *priv = netdev_priv(dev);

	if (unlikely(!dev)) {
		pr_err("%s: invalid dev pointer\n", __func__);
		return IRQ_NONE;
	}

	if (priv->is_gmac) {
		unsigned long ioaddr = dev->base_addr;
		/* To handle GMAC own interrupts */
		priv->mac_type->ops->host_irq_status(ioaddr);
	}
	stmmac_dma_interrupt(dev);

	return IRQ_HANDLED;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
/* Polling receive - used by NETCONSOLE and other diagnostic tools
 * to allow network I/O with interrupts disabled. */
static void stmmac_poll_controller(struct net_device *dev)
{
	disable_irq(dev->irq);
	stmmac_interrupt(dev->irq, dev);
	enable_irq(dev->irq);
}
#endif

/**
 *  stmmac_ioctl - Entry point for the Ioctl
 *  @dev :  Device pointer.
 *  @rq :  An IOCTL specefic structure, that can contain a pointer to
 *  a proprietary structure used to pass information to the driver.
 *  @cmd :  IOCTL command
 *  Description:
 *  Currently there are no special functionality supported in IOCTL, just the
 *  phy_mii_ioctl(...) can be invoked.
 */
static int stmmac_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	int ret = -EOPNOTSUPP;

	if (!netif_running(dev))
		return -EINVAL;

	switch (cmd) {
	case SIOCGMIIPHY:
	case SIOCGMIIREG:
	case SIOCSMIIREG:
		if (!priv->phydev)
			return -EINVAL;

		spin_lock(&priv->lock);
		ret = phy_mii_ioctl(priv->phydev, if_mii(rq), cmd);
		spin_unlock(&priv->lock);
	default:
		break;
	}
	return ret;
}

#ifdef STMMAC_VLAN_TAG_USED
static void stmmac_vlan_rx_register(struct net_device *dev,
				    struct vlan_group *grp)
{
	struct stmmac_priv *priv = netdev_priv(dev);

	DBG(probe, INFO, "%s: Setting vlgrp to %p\n", dev->name, grp);

	spin_lock(&priv->lock);
	priv->vlgrp = grp;
	spin_unlock(&priv->lock);

	return;
}

static void stmmac_vlan_rx_add_vid(struct net_device *dev, unsigned short vid)
{
	struct stmmac_priv *priv = netdev_priv(dev);

	DBG(probe, INFO, "%s: Adding vlanid %d to vlan filter\n", dev->name,
	    vid);
	spin_lock(&priv->lock);
	priv->mac_type->ops->set_filter(dev);
	spin_unlock(&priv->lock);
	return;
}

static void stmmac_vlan_rx_kill_vid(struct net_device *dev, unsigned short vid)
{
	struct stmmac_priv *priv = netdev_priv(dev);

	DBG(probe, INFO, "%s: removing vlanid %d from vlan filter\n",
	    dev->name, vid);

	spin_lock(&priv->lock);
	vlan_group_set_device(priv->vlgrp, vid, NULL);
	priv->mac_type->ops->set_filter(dev);
	spin_unlock(&priv->lock);
	return;
}
#endif

static const struct net_device_ops stmmac_netdev_ops = {
	.ndo_open = stmmac_open,
	.ndo_start_xmit = stmmac_xmit,
	.ndo_stop = stmmac_release,
	.ndo_change_mtu = stmmac_change_mtu,
	.ndo_set_multicast_list = stmmac_multicast_list,
	.ndo_tx_timeout = stmmac_tx_timeout,
	.ndo_do_ioctl = stmmac_ioctl,
	.ndo_set_config = stmmac_config,
#ifdef STMMAC_VLAN_TAG_USED
	.ndo_vlan_rx_register = stmmac_vlan_rx_register,
#endif
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller = stmmac_poll_controller,
#endif
};


/**
 *  stmmac_probe - Initialization of the adapter .
 *  @dev : device pointer
 *  Description: The function initializes the network device structure for
 *		the STMMAC driver. It also calls the low level routines
 *		 in order to init the HW (i.e. the DMA engine)
 */
static int stmmac_probe(struct net_device *dev)
{
	int ret = 0;
	struct stmmac_priv *priv = netdev_priv(dev);

	ether_setup(dev);

	dev->netdev_ops = &stmmac_netdev_ops;
	dev->ethtool_ops = &stmmac_ethtool_ops;

	dev->features |= (NETIF_F_SG | NETIF_F_HW_CSUM | NETIF_F_HIGHDMA);
	dev->watchdog_timeo = msecs_to_jiffies(watchdog);
#ifdef STMMAC_VLAN_TAG_USED
	/* Both mac100 and gmac support receive VLAN tag detection */
	dev->features |= NETIF_F_HW_VLAN_RX;

	if (priv->vlan_rx_filter) { /*FIXME*/
		dev->features |= NETIF_F_HW_VLAN_FILTER;
		dev->vlan_rx_add_vid = stmmac_vlan_rx_add_vid;
		dev->vlan_rx_kill_vid = stmmac_vlan_rx_kill_vid;
	}
#endif
	priv->msg_enable = netif_msg_init(debug, default_msg_level);

	if (priv->is_gmac)
		priv->rx_csum = 1;

	if (flow_ctrl)
		priv->flow_ctrl = FLOW_AUTO;	/* RX/TX pause on */

	priv->pause = pause;
	netif_napi_add(dev, &priv->napi, stmmac_poll, 64);

	/* Get the MAC address */
	priv->mac_type->ops->get_umac_addr(dev->base_addr, dev->dev_addr, 0);

	stmmac_init_coalescence(priv->is_gmac, dev->mtu);

	if (!is_valid_ether_addr(dev->dev_addr))
		pr_warning("\tno valid MAC address;"
			"please, use ifconfig or nwhwconfig!\n");

	ret = register_netdev(dev);
	if (ret) {
		pr_err("%s: ERROR %i registering the device\n",
		       __func__, ret);
		return -ENODEV;
	}

	DBG(probe, DEBUG, "%s: Scatter/Gather: %s - HW checksums: %s\n",
	    dev->name, (dev->features & NETIF_F_SG) ? "on" : "off",
	    (dev->features & NETIF_F_HW_CSUM) ? "on" : "off");

	spin_lock_init(&priv->lock);
	spin_lock_init(&priv->tx_lock);

	return ret;
}

/**
 * stmmac_mac_device_setup
 * @dev : device pointer
 * Description: it detects and inits either the mac 10/100 or the Gmac.
 */
static void stmmac_mac_device_setup(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;

	struct mac_device_info *device;

	if (priv->is_gmac)
		device = gmac_setup(ioaddr);
	else
		device = mac100_setup(ioaddr);
	priv->mac_type = device;
	priv->wolenabled = priv->mac_type->hw.pmt;	/* PMT supported */

	return;
}

 /**
 * stmmacphy_dvr_probe
 * @pdev: platform device pointer
 * Description: The driver is initialized through the platform_device
 * 		structures which define the configuration needed by the SoC.
 *		These are defined in arch/sh/kernel/cpu/sh4
 */
static int stmmacphy_dvr_probe(struct platform_device *pdev)
{
	struct plat_stmmacphy_data *plat_dat;
	plat_dat = (struct plat_stmmacphy_data *)((pdev->dev).platform_data);

	pr_debug("stmmacphy_dvr_probe: added phy for bus %d\n",
	       plat_dat->bus_id);

	return 0;
}

static int stmmacphy_dvr_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver stmmacphy_driver = {
	.driver = {
		   .name = PHY_RESOURCE_NAME,
		   },
	.probe = stmmacphy_dvr_probe,
	.remove = stmmacphy_dvr_remove,
};

/**
 * stmmac_associate_phy
 * @dev: pointer to device structure
 * @data: points to the private structure.
 * Description: Scans through all the PHYs we have registered and checks if
 *		any are associated with our MAC.  If so, then just fill in
 *		the blanks in our local context structure
 */
static int stmmac_associate_phy(struct device *dev, void *data)
{
	struct stmmac_priv *priv = (struct stmmac_priv *)data;
	struct plat_stmmacphy_data *plat_dat;

	plat_dat = (struct plat_stmmacphy_data *)(dev->platform_data);

	DBG(probe, DEBUG, "%s: checking phy for bus %d\n", __func__,
		plat_dat->bus_id);

	/* Check that this phy is for the MAC being initialised */
	if (priv->bus_id != plat_dat->bus_id)
		return 0;

	/* OK, this PHY is connected to the MAC.
	   Go ahead and get the parameters */
	DBG(probe, DEBUG, "%s: OK. Found PHY config\n", __func__);
	priv->phy_irq =
	    platform_get_irq_byname(to_platform_device(dev), "phyirq");
	DBG(probe, DEBUG, "%s: PHY irq on bus %d is %d\n", __func__,
	    plat_dat->bus_id, priv->phy_irq);

	/* Override with kernel parameters if supplied XXX CRS XXX
	 * this needs to have multiple instances */
	if ((phyaddr >= 0) && (phyaddr <= 31))
		plat_dat->phy_addr = phyaddr;

	priv->phy_addr = plat_dat->phy_addr;
	priv->phy_mask = plat_dat->phy_mask;
	priv->phy_interface = plat_dat->interface;
	priv->phy_reset = plat_dat->phy_reset;

	DBG(probe, DEBUG, "%s: exiting\n", __func__);
	return 1;		/* forces exit of driver_for_each_device() */
}

/**
 * stmmac_dvr_probe
 * @pdev: platform device pointer
 * Description: The driver is initialized through platform_device.
 * 		Structures which define the configuration needed by the board
 *		are defined in a board structure in arch/sh/boards/st/ .
 */
static int stmmac_dvr_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct resource *res;
	unsigned int *addr = NULL;
	struct net_device *ndev = NULL;
	struct stmmac_priv *priv;
	struct plat_stmmacenet_data *plat_dat;

	pr_info("STMMAC driver:\n\tplatform registration... ");
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto out;
	}
	pr_info("done!\n");

	if (!request_mem_region(res->start, (res->end - res->start),
				pdev->name)) {
		pr_err("%s: ERROR: memory allocation failed"
		       "cannot get the I/O addr 0x%x\n",
		       __func__, (unsigned int)res->start);
		ret = -EBUSY;
		goto out;
	}

	addr = ioremap(res->start, (res->end - res->start));
	if (!addr) {
		pr_err("%s: ERROR: memory mapping failed \n", __func__);
		ret = -ENOMEM;
		goto out;
	}

	ndev = alloc_etherdev(sizeof(struct stmmac_priv));
	if (!ndev) {
		pr_err("%s: ERROR: allocating the device\n", __func__);
		ret = -ENOMEM;
		goto out;
	}

	SET_NETDEV_DEV(ndev, &pdev->dev);

	/* Get the MAC information */
	ndev->irq = platform_get_irq_byname(pdev, "macirq");
	if (ndev->irq == -ENXIO) {
		pr_err("%s: ERROR: MAC IRQ configuration "
		       "information not found\n", __func__);
		ret = -ENODEV;
		goto out;
	}

	priv = netdev_priv(ndev);
	priv->device = &(pdev->dev);
	priv->dev = ndev;
	plat_dat = (struct plat_stmmacenet_data *)((pdev->dev).platform_data);
	priv->bus_id = plat_dat->bus_id;
	priv->pbl = plat_dat->pbl;	/* TLI */
	priv->is_gmac = plat_dat->has_gmac;	/* GMAC is on board */
	priv->vlan_rx_filter = 0;	/*plat_dat->vlan_rx_filter; */

	platform_set_drvdata(pdev, ndev);

	/* Set the I/O base addr */
	ndev->base_addr = (unsigned long)addr;

	/* MAC HW revice detection */
	stmmac_mac_device_setup(ndev);

	/* Network Device Registration */
	ret = stmmac_probe(ndev);
	if (ret < 0)
		goto out;

	/* associate a PHY - it is provided by another platform bus */
	if (!driver_for_each_device
	    (&(stmmacphy_driver.driver), NULL, (void *)priv,
	     stmmac_associate_phy)) {
		pr_err("No PHY device is associated with this MAC!\n");
		ret = -ENODEV;
		goto out;
	}

	priv->fix_mac_speed = plat_dat->fix_mac_speed;
	priv->bsp_priv = plat_dat->bsp_priv;

	pr_info("\t%s - (dev. name: %s - id: %d, IRQ #%d\n"
	       "\tIO base addr: 0x%08x)\n", ndev->name, pdev->name,
	       pdev->id, ndev->irq, (unsigned int)addr);

	/* MDIO bus Registration */
	pr_debug("\tMDIO bus (id: %d)...", priv->bus_id);
	ret = stmmac_mdio_register(ndev);
	if (ret < 0)
		goto out;
	pr_debug("registered!\n");

out:
	if (ret < 0) {
		platform_set_drvdata(pdev, NULL);
		release_mem_region(res->start, (res->end - res->start));
		if (addr != NULL)
			iounmap(addr);
	}

	return ret;
}

/**
 * stmmac_dvr_remove
 * @pdev: platform device pointer
 * Description: This function resets the TX/RX processes, disables the MAC RX/TX
 *   changes the link status, releases the DMA descriptor rings,
 *   unregisters the MDIO busm and unmaps allocated memory.
 */
static int stmmac_dvr_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct resource *res;

	pr_info("%s:\n\tremoving driver", __func__);

	stmmac_dma_stop_rx(ndev->base_addr);
	stmmac_dma_stop_tx(ndev->base_addr);

	stmmac_mac_disable_rx(ndev);
	stmmac_mac_disable_tx(ndev);

	netif_carrier_off(ndev);

	stmmac_mdio_unregister(ndev);

	platform_set_drvdata(pdev, NULL);
	unregister_netdev(ndev);

	iounmap((void *)ndev->base_addr);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	release_mem_region(res->start, (res->end - res->start));

	free_netdev(ndev);

	return 0;
}

#ifdef CONFIG_PM
static int stmmac_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct stmmac_priv *priv = netdev_priv(dev);

	if (!dev || !netif_running(dev))
		return 0;

	spin_lock(&priv->lock);

	if (state.event == PM_EVENT_SUSPEND) {
		netif_device_detach(dev);
		netif_stop_queue(dev);
		if (priv->phydev)
			phy_stop(priv->phydev);
		netif_stop_queue(dev);
		tasklet_disable(&priv->tx_task);

#ifdef CONFIG_STMMAC_TIMER
		priv->tm->timer_stop();
#endif
		napi_disable(&priv->napi);

		/* Stop TX/RX DMA */
		stmmac_dma_stop_tx(dev->base_addr);
		stmmac_dma_stop_rx(dev->base_addr);
		/* Clear the Rx/Tx descriptors */
		priv->mac_type->ops->init_rx_desc(priv->dma_rx,
						  priv->dma_rx_size);
		priv->mac_type->ops->disable_rx_ic(priv->dma_rx,
						   priv->dma_rx_size,
						   rx_coalesce);
		priv->mac_type->ops->init_tx_desc(priv->dma_tx,
						  priv->dma_tx_size);

		stmmac_mac_disable_tx(dev);

		if (device_may_wakeup(&(pdev->dev))) {
			/* Enable Power down mode by programming the PMT regs */
			if (priv->wolenabled == PMT_SUPPORTED)
				priv->mac_type->ops->pmt(dev->base_addr,
							 priv->wolopts);
		} else {
			stmmac_mac_disable_rx(dev);
		}
	} else {
		priv->shutdown = 1;
		/* Although this can appear slightly redundant it actually
		 * makes fast the standby operation and guarantees the driver
		 * working if hibernation is on media. */
		stmmac_release(dev);
	}

	spin_unlock(&priv->lock);
	return 0;
}

static int stmmac_resume(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct stmmac_priv *priv = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;

	if (!netif_running(dev))
		return 0;

	spin_lock(&priv->lock);

	if (priv->shutdown) {
		/* Re-open the interface and re-init the MAC/DMA
		   and the rings. */
		stmmac_open(dev);
		goto out_resume;
	}

	/* Power Down bit, into the PM register, is cleared
	 * automatically as soon as a magic packet or a Wake-up frame
	 * is received. Anyway, it's better to manually clear
	 * this bit because it can generate problems while resuming
	 * from another devices (e.g. serial console). */
	if (device_may_wakeup(&(pdev->dev)))
		if (priv->wolenabled == PMT_SUPPORTED)
			priv->mac_type->ops->pmt(dev->base_addr, 0);

	netif_device_attach(dev);

	/* Enable the MAC and DMA */
	stmmac_mac_enable_rx(dev);
	stmmac_mac_enable_tx(dev);
	stmmac_dma_start_tx(ioaddr);
	stmmac_dma_start_rx(ioaddr);

#ifdef CONFIG_STMMAC_TIMER
	priv->tm->timer_start(tmrate);
#endif
	napi_enable(&priv->napi);

	tasklet_enable(&priv->tx_task);

	if (priv->phydev)
		phy_start(priv->phydev);

	netif_start_queue(dev);

out_resume:
	spin_unlock(&priv->lock);
	return 0;
}
#endif

static struct platform_driver stmmac_driver = {
	.driver = {
		   .name = STMMAC_RESOURCE_NAME,
		   },
	.probe = stmmac_dvr_probe,
	.remove = stmmac_dvr_remove,
#ifdef CONFIG_PM
	.suspend = stmmac_suspend,
	.resume = stmmac_resume,
#endif

};

/**
 * stmmac_init_module - Entry point for the driver
 * Description: This function is the entry point for the driver.
 * It returns error if the mac core registration fails.
 */
static int __init stmmac_init_module(void)
{
	int ret;

	if (platform_driver_register(&stmmacphy_driver)) {
		pr_err("No PHY devices registered!\n");
		return -ENODEV;
	}

	ret = platform_driver_register(&stmmac_driver);
	return ret;
}

/**
 * stmmac_cleanup_module - Cleanup routine for the driver
 * Description: This function is the cleanup routine for the driver.
 */
static void __exit stmmac_cleanup_module(void)
{
	platform_driver_unregister(&stmmacphy_driver);
	platform_driver_unregister(&stmmac_driver);
}

#ifndef MODULE
static int __init stmmac_cmdline_opt(char *str)
{
	char *opt;

	if (!str || !*str)
		return -EINVAL;
	while ((opt = strsep(&str, ",")) != NULL) {
		if (!strncmp(opt, "debug:", 6))
			strict_strtoul(opt + 6, 0, (unsigned long *)&debug);
		else if (!strncmp(opt, "phyaddr:", 8))
			strict_strtoul(opt + 8, 0, (unsigned long *)&phyaddr);
		else if (!strncmp(opt, "dma_txsize:", 11))
			strict_strtoul(opt + 11, 0,
				       (unsigned long *)&dma_txsize);
		else if (!strncmp(opt, "dma_rxsize:", 11))
			strict_strtoul(opt + 11, 0,
				       (unsigned long *)&dma_rxsize);
		else if (!strncmp(opt, "buf_sz:", 7))
			strict_strtoul(opt + 7, 0, (unsigned long *)&buf_sz);
		else if (!strncmp(opt, "tc:", 3))
			strict_strtoul(opt + 3, 0, (unsigned long *)&tc);
		else if (!strncmp(opt, "tx_coe:", 7))
			strict_strtoul(opt + 7, 0, (unsigned long *)&tx_coe);
		else if (!strncmp(opt, "watchdog:", 9))
			strict_strtoul(opt + 9, 0, (unsigned long *)&watchdog);
		else if (!strncmp(opt, "flow_ctrl:", 10))
			strict_strtoul(opt + 10, 0,
				       (unsigned long *)&flow_ctrl);
		else if (!strncmp(opt, "pause:", 6))
			strict_strtoul(opt + 6, 0, (unsigned long *)&pause);
		else if (!strncmp(opt, "tx_coalesce:", 12))
			strict_strtoul(opt + 12, 0,
				       (unsigned long *)&tx_coalesce);
		else if (!strncmp(opt, "rx_coalesce:", 12))
			strict_strtoul(opt + 12, 0,
				       (unsigned long *)&rx_coalesce);
#ifdef CONFIG_STMMAC_TIMER
		else if (!strncmp(opt, "tmrate:", 7))
			strict_strtoul(opt + 7, 0, (unsigned long *)&tmrate);
#endif
	}
	return 0;
}

__setup("stmmaceth=", stmmac_cmdline_opt);
#endif

module_init(stmmac_init_module);
module_exit(stmmac_cleanup_module);

MODULE_DESCRIPTION("STMMAC 10/100/1000 Ethernet driver");
MODULE_AUTHOR("Giuseppe Cavallaro <peppe.cavallaro@st.com>");
MODULE_LICENSE("GPL");
