// SPDX-License-Identifier: GPL-2.0-only
/*
 * EcoNet / MediaTek MT7510 frame engine, PDMA mode.
 *
 * On MT7510 the LAN path of the frame engine (GDMA1) is fed by the older
 * Ralink style PDMA, not by QDMA. Register usage, descriptor layout and init
 * order follow the stock tc3262_gmac driver (raeth.ko, "7510FE_2.0").
 *
 * One TX ring, one RX ring, NAPI. The internal switch is left as a dumb
 * switch with port 6 as the CPU port (same values as the bootloader/stock).
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <net/dsa.h>

/* Frame engine registers (offsets from 0x1fb50000) */
#define FE_GLO_108		0x108
#define FE_GLO_10C		0x10c
#define CDMA_CSG_CFG		0x400
#define GDMA1_FWD_CFG		0x500
#define  GDM_FWD_PORT_MASK	0x7777		/* 0 = CPU (PDMA) */
#define  GDM_STRP_CRC		BIT(16)
#define  GDM_UCS_EN		BIT(20)
#define  GDM_TCS_EN		BIT(21)
#define  GDM_ICS_EN		BIT(22)
#define  GDM_STAG_EN		BIT(24)
#define GDMA1_MAC_ADRL		0x508
#define GDMA1_MAC_ADRH		0x50c
#define GDMA1_LEN_CFG		0x514		/* max[31:16] min[15:0], default 1514/60 */
#define  GDMA1_LEN_VAL		((RX_BUF_LEN << 16) | 60)

#define PDMA_TX_BASE		0x800
#define PDMA_TX_MAX_CNT		0x804
#define PDMA_TX_CTX_IDX		0x808
#define PDMA_TX_DTX_IDX		0x80c
#define PDMA_RX_BASE		0x900
#define PDMA_RX_MAX_CNT		0x904
#define PDMA_RX_CALC_IDX	0x908
#define PDMA_RX_DRX_IDX		0x90c
#define PDMA_GLO_CFG		0xa04
#define  PDMA_TX_EN		BIT(0)
#define  PDMA_RX_EN		BIT(2)
#define  PDMA_TX_WB_DDONE	BIT(6)
#define  PDMA_BT_SIZE_MASK	GENMASK(5, 4)
#define  PDMA_START_BITS	0xa0000075	/* stock value */
#define PDMA_RST_IDX		0xa08
#define  PDMA_RST_TX0		BIT(0)
#define  PDMA_RST_RX0		BIT(16)
#define FE_INT_STATUS		0xa20
#define FE_INT_ENABLE		0xa28
#define  INT_TX_DONE0		BIT(0)
#define  INT_RX_DONE0		BIT(16)
#define  INT_RX_DONE1		BIT(17)
#define  INT_RX_DLY		BIT(30)
#define  INT_RX_BITS		(INT_RX_DLY | INT_RX_DONE1 | INT_RX_DONE0)
#define  INT_ERR_BITS		(BIT(31) | BIT(29))

/* Internal switch (MT7530 style) at 0x1fb58000 */
#define GSW_BASE		0x8000
#define GSW_MFC			(GSW_BASE + 0x0010)
#define GSW_000C		(GSW_BASE + 0x000c)
#define GSW_P6_PVC		(GSW_BASE + 0x2610)
#define  PVC_PORT_STAG		BIT(5)
#define GSW_P6_PMCR		(GSW_BASE + 0x3600)
#define GSW_SMACCR0		(GSW_BASE + 0x30e4)
#define GSW_SMACCR1		(GSW_BASE + 0x30e8)
#define  GSW_P6_PMCR_VAL	0x0005e33b	/* stock: forced 1G FD, fc */
#define  GSW_MFC_VAL		0xffffffe0	/* flood all, CPU port 6 */

/* Descriptors: 4 x u32, CPU (big-endian) order */
#define DESC_DDONE		BIT(31)
#define DESC_LS0		BIT(30)
#define DESC_PLEN0(x)		(((x) & 0x3fff) << 16)
#define DESC_PLEN0_GET(x)	(((x) >> 16) & 0x3fff)
#define TXD4_FPORT(x)		(((x) & 0x7) << 19)
#define  FPORT_GDMA1		1

#define NUM_TX_DESC		256
#define NUM_RX_DESC		256
#define RX_BUF_LEN		1536
#define NAPI_WEIGHT		64

struct pdma_desc {
	u32 w0;		/* buffer address */
	u32 w1;		/* DDONE / LS0 / PLEN0 */
	u32 w2;
	u32 w3;		/* TXD4: forward port, csum */
};

struct mt7510_eth {
	void __iomem		*base;
	bool			has_gsw;	/* switch regs inside our mapping */
	struct device		*dev;
	struct net_device	*ndev;
	struct napi_struct	napi;
	int			irq;
	spinlock_t		tx_lock;

	struct pdma_desc	*tx_ring;
	dma_addr_t		tx_phys;
	struct sk_buff		*tx_skb[NUM_TX_DESC];
	dma_addr_t		tx_dma[NUM_TX_DESC];
	unsigned int		tx_head;	/* next to fill */
	unsigned int		tx_tail;	/* next to reclaim */

	struct pdma_desc	*rx_ring;
	dma_addr_t		rx_phys;
	struct sk_buff		*rx_skb[NUM_RX_DESC];
	dma_addr_t		rx_dma[NUM_RX_DESC];
	unsigned int		rx_idx;

	u32			int_err;
};

static inline u32 fe_r32(struct mt7510_eth *eth, u32 reg)
{
	return readl(eth->base + reg);
}

static inline void fe_w32(struct mt7510_eth *eth, u32 val, u32 reg)
{
	writel(val, eth->base + reg);
}

static inline void fe_rmw(struct mt7510_eth *eth, u32 reg, u32 clr, u32 set)
{
	fe_w32(eth, (fe_r32(eth, reg) & ~clr) | set, reg);
}

static void mt7510_irq_rx(struct mt7510_eth *eth, bool on)
{
	if (on)
		fe_rmw(eth, FE_INT_ENABLE, 0, INT_RX_BITS | INT_TX_DONE0);
	else
		fe_rmw(eth, FE_INT_ENABLE, INT_RX_BITS | INT_TX_DONE0, 0);
}

static int mt7510_rx_refill(struct mt7510_eth *eth, unsigned int i)
{
	struct sk_buff *skb;
	dma_addr_t dma;

	skb = netdev_alloc_skb_ip_align(eth->ndev, RX_BUF_LEN);
	if (!skb)
		return -ENOMEM;

	dma = dma_map_single(eth->dev, skb->data, RX_BUF_LEN, DMA_FROM_DEVICE);
	if (dma_mapping_error(eth->dev, dma)) {
		dev_kfree_skb_any(skb);
		return -ENOMEM;
	}

	eth->rx_skb[i] = skb;
	eth->rx_dma[i] = dma;
	eth->rx_ring[i].w0 = dma;
	eth->rx_ring[i].w2 = 0;
	eth->rx_ring[i].w3 = 0;
	wmb();
	eth->rx_ring[i].w1 = DESC_LS0 | DESC_PLEN0(RX_BUF_LEN);
	return 0;
}

static void mt7510_tx_reclaim(struct mt7510_eth *eth)
{
	unsigned int done = 0, bytes = 0;

	spin_lock(&eth->tx_lock);
	while (eth->tx_tail != eth->tx_head) {
		unsigned int i = eth->tx_tail;
		struct sk_buff *skb = eth->tx_skb[i];

		if (!(READ_ONCE(eth->tx_ring[i].w1) & DESC_DDONE))
			break;

		if (skb) {
			dma_unmap_single(eth->dev, eth->tx_dma[i], skb->len,
					 DMA_TO_DEVICE);
			bytes += skb->len;
			dev_consume_skb_any(skb);
			eth->tx_skb[i] = NULL;
		}
		done++;
		eth->tx_tail = (i + 1) % NUM_TX_DESC;
	}

	if (done) {
		eth->ndev->stats.tx_packets += done;
		eth->ndev->stats.tx_bytes += bytes;
		if (netif_queue_stopped(eth->ndev))
			netif_wake_queue(eth->ndev);
	}
	spin_unlock(&eth->tx_lock);
}

static int mt7510_poll(struct napi_struct *napi, int budget)
{
	struct mt7510_eth *eth = container_of(napi, struct mt7510_eth, napi);
	struct net_device *ndev = eth->ndev;
	int work = 0;

	mt7510_tx_reclaim(eth);

	while (work < budget) {
		unsigned int i = eth->rx_idx;
		struct pdma_desc *d = &eth->rx_ring[i];
		struct sk_buff *skb = eth->rx_skb[i];
		dma_addr_t dma = eth->rx_dma[i];
		u32 w1 = READ_ONCE(d->w1);
		unsigned int len;

		if (!(w1 & DESC_DDONE))
			break;
		rmb();

		len = DESC_PLEN0_GET(w1);
		if (len < ETH_HLEN || len > RX_BUF_LEN ||
		    mt7510_rx_refill(eth, i)) {
			/* drop: hand the same buffer back */
			ndev->stats.rx_dropped++;
			d->w0 = dma;
			wmb();
			d->w1 = DESC_LS0 | DESC_PLEN0(RX_BUF_LEN);
		} else {
			dma_unmap_single(eth->dev, dma, RX_BUF_LEN,
					 DMA_FROM_DEVICE);
			skb_put(skb, len);
			skb->protocol = eth_type_trans(skb, ndev);
			ndev->stats.rx_packets++;
			ndev->stats.rx_bytes += len;
			napi_gro_receive(napi, skb);
		}

		wmb();
		fe_w32(eth, i, PDMA_RX_CALC_IDX);
		eth->rx_idx = (i + 1) % NUM_RX_DESC;
		work++;
	}

	if (work < budget && napi_complete_done(napi, work))
		mt7510_irq_rx(eth, true);

	return work;
}

static irqreturn_t mt7510_isr(int irq, void *data)
{
	struct mt7510_eth *eth = data;
	u32 st = fe_r32(eth, FE_INT_STATUS);

	if (!st)
		return IRQ_NONE;

	fe_w32(eth, st, FE_INT_STATUS);

	if (st & INT_ERR_BITS)
		eth->int_err++;

	if (st & (INT_RX_BITS | INT_TX_DONE0)) {
		mt7510_irq_rx(eth, false);
		napi_schedule(&eth->napi);
	}

	return IRQ_HANDLED;
}

static netdev_tx_t mt7510_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct mt7510_eth *eth = netdev_priv(ndev);
	struct pdma_desc *d;
	unsigned int i, next;
	dma_addr_t dma;

	if (eth_skb_pad(skb)) {
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	spin_lock_bh(&eth->tx_lock);

	i = eth->tx_head;
	next = (i + 1) % NUM_TX_DESC;
	d = &eth->tx_ring[i];

	if (next == eth->tx_tail || !(READ_ONCE(d->w1) & DESC_DDONE)) {
		netif_stop_queue(ndev);
		spin_unlock_bh(&eth->tx_lock);
		return NETDEV_TX_BUSY;
	}

	dma = dma_map_single(eth->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(eth->dev, dma)) {
		spin_unlock_bh(&eth->tx_lock);
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	eth->tx_skb[i] = skb;
	eth->tx_dma[i] = dma;
	d->w0 = dma;
	d->w2 = 0;
	d->w3 = TXD4_FPORT(FPORT_GDMA1);
	wmb();
	d->w1 = DESC_LS0 | DESC_PLEN0(skb->len);	/* clears DDONE */
	wmb();

	eth->tx_head = next;
	fe_w32(eth, next, PDMA_TX_CTX_IDX);

	if ((eth->tx_head + 1) % NUM_TX_DESC == eth->tx_tail)
		netif_stop_queue(ndev);

	spin_unlock_bh(&eth->tx_lock);

	return NETDEV_TX_OK;
}

static void mt7510_set_hw_mac(struct mt7510_eth *eth, const u8 *a)
{
	u32 lo = (a[2] << 24) | (a[3] << 16) | (a[4] << 8) | a[5];
	u32 hi = (a[0] << 8) | a[1];

	fe_w32(eth, lo, GDMA1_MAC_ADRL);
	fe_w32(eth, hi, GDMA1_MAC_ADRH);
	fe_w32(eth, lo, GSW_SMACCR0);
	fe_w32(eth, hi, GSW_SMACCR1);
}

static int mt7510_set_mac(struct net_device *ndev, void *p)
{
	struct mt7510_eth *eth = netdev_priv(ndev);
	int ret = eth_mac_addr(ndev, p);

	if (!ret)
		mt7510_set_hw_mac(eth, ndev->dev_addr);
	return ret;
}

static void mt7510_free_rings(struct mt7510_eth *eth)
{
	int i;

	for (i = 0; i < NUM_RX_DESC; i++) {
		if (!eth->rx_skb[i])
			continue;
		dma_unmap_single(eth->dev, eth->rx_dma[i], RX_BUF_LEN,
				 DMA_FROM_DEVICE);
		dev_kfree_skb_any(eth->rx_skb[i]);
		eth->rx_skb[i] = NULL;
	}
	for (i = 0; i < NUM_TX_DESC; i++) {
		if (!eth->tx_skb[i])
			continue;
		dma_unmap_single(eth->dev, eth->tx_dma[i],
				 eth->tx_skb[i]->len, DMA_TO_DEVICE);
		dev_kfree_skb_any(eth->tx_skb[i]);
		eth->tx_skb[i] = NULL;
	}
}

static void mt7510_dma_stop(struct mt7510_eth *eth)
{
	int i;

	fe_rmw(eth, PDMA_GLO_CFG, PDMA_TX_EN | PDMA_RX_EN | PDMA_TX_WB_DDONE, 0);
	/* wait for the busy bits (1, 3) to clear */
	for (i = 0; i < 100; i++) {
		if (!(fe_r32(eth, PDMA_GLO_CFG) & (BIT(1) | BIT(3))))
			break;
		usleep_range(100, 200);
	}
}

static int mt7510_hw_init(struct mt7510_eth *eth)
{
	int i, err;

	mt7510_dma_stop(eth);
	fe_w32(eth, 0, FE_INT_ENABLE);
	fe_w32(eth, ~0u, FE_INT_STATUS);

	/* TX ring: all owned by CPU (DDONE), last segment, out via GDMA1 */
	for (i = 0; i < NUM_TX_DESC; i++) {
		eth->tx_ring[i].w0 = 0;
		eth->tx_ring[i].w1 = DESC_DDONE | DESC_LS0;
		eth->tx_ring[i].w2 = 0;
		eth->tx_ring[i].w3 = TXD4_FPORT(FPORT_GDMA1);
	}
	eth->tx_head = eth->tx_tail = 0;

	for (i = 0; i < NUM_RX_DESC; i++) {
		err = mt7510_rx_refill(eth, i);
		if (err)
			return err;
	}
	eth->rx_idx = 0;
	wmb();

	/* macSetDMADescrCtrlReg */
	fe_w32(eth, eth->tx_phys, PDMA_TX_BASE);
	fe_w32(eth, NUM_TX_DESC, PDMA_TX_MAX_CNT);
	fe_w32(eth, 0, PDMA_TX_CTX_IDX);
	fe_w32(eth, PDMA_RST_TX0, PDMA_RST_IDX);
	fe_w32(eth, eth->rx_phys, PDMA_RX_BASE);
	fe_w32(eth, NUM_RX_DESC, PDMA_RX_MAX_CNT);
	fe_w32(eth, NUM_RX_DESC - 1, PDMA_RX_CALC_IDX);
	fe_w32(eth, PDMA_RST_RX0, PDMA_RST_IDX);

	/* macDrvRegInit: GDMA1 -> CPU for all traffic, strip CRC, rx csum */
	fe_rmw(eth, GDMA1_FWD_CFG, GDM_FWD_PORT_MASK | GDM_STAG_EN,
	       GDM_STRP_CRC | GDM_UCS_EN | GDM_TCS_EN | GDM_ICS_EN);
	fe_w32(eth, 0x81000000, CDMA_CSG_CFG);	/* TPID 0x8100, no stag */
	fe_w32(eth, 0x00304818, FE_GLO_108);
	fe_w32(eth, 0x00004028, FE_GLO_10C);
	/* Accept frames with a VLAN or MTK switch tag (default max is 1514) */
	fe_w32(eth, GDMA1_LEN_VAL, GDMA1_LEN_CFG);

	mt7510_set_hw_mac(eth, eth->ndev->dev_addr);

	/*
	 * macSetGSW, minus special tag: plain dumb switch, CPU on port 6.
	 * When a DSA switch driver owns the switch, leave it alone.
	 */
	if (eth->has_gsw && !netdev_uses_dsa(eth->ndev)) {
		fe_rmw(eth, GSW_P6_PVC, PVC_PORT_STAG, 0);
		fe_w32(eth, GSW_P6_PMCR_VAL, GSW_P6_PMCR);
		fe_w32(eth, GSW_MFC_VAL, GSW_MFC);
		fe_rmw(eth, GSW_000C, BIT(4), 0);
	}

	return 0;
}

static int mt7510_open(struct net_device *ndev)
{
	struct mt7510_eth *eth = netdev_priv(ndev);
	int err;

	err = mt7510_hw_init(eth);
	if (err) {
		mt7510_free_rings(eth);
		return err;
	}

	napi_enable(&eth->napi);
	mt7510_irq_rx(eth, true);
	fe_rmw(eth, PDMA_GLO_CFG, PDMA_BT_SIZE_MASK, PDMA_START_BITS);

	netif_carrier_on(ndev);
	netif_start_queue(ndev);

	dev_info(eth->dev, "up: GLO %08x FWD %08x INT_EN %08x\n",
		 fe_r32(eth, PDMA_GLO_CFG), fe_r32(eth, GDMA1_FWD_CFG),
		 fe_r32(eth, FE_INT_ENABLE));
	return 0;
}

static int mt7510_stop(struct net_device *ndev)
{
	struct mt7510_eth *eth = netdev_priv(ndev);

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	fe_w32(eth, 0, FE_INT_ENABLE);
	napi_disable(&eth->napi);
	mt7510_dma_stop(eth);
	mt7510_free_rings(eth);
	return 0;
}

static const struct net_device_ops mt7510_netdev_ops = {
	.ndo_open		= mt7510_open,
	.ndo_stop		= mt7510_stop,
	.ndo_start_xmit		= mt7510_xmit,
	.ndo_set_mac_address	= mt7510_set_mac,
	.ndo_validate_addr	= eth_validate_addr,
};

static int mt7510_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *rst;
	struct device_node *mac_np;
	struct net_device *ndev;
	struct mt7510_eth *eth;
	struct resource *res;
	int err;

	ndev = devm_alloc_etherdev(dev, sizeof(*eth));
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, dev);
	eth = netdev_priv(ndev);
	eth->dev = dev;
	eth->ndev = ndev;
	spin_lock_init(&eth->tx_lock);

	eth->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(eth->base))
		return PTR_ERR(eth->base);
	/* With a DSA switch node the switch registers are not ours */
	eth->has_gsw = resource_size(res) > GSW_BASE;

	eth->irq = platform_get_irq(pdev, 0);
	if (eth->irq < 0)
		return eth->irq;

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (err)
		return err;

	/* stock macReset: assert 200 ms, release, settle 50 ms */
	rst = devm_reset_control_array_get_optional_exclusive(dev);
	if (IS_ERR(rst))
		return PTR_ERR(rst);
	if (rst) {
		reset_control_assert(rst);
		msleep(200);
		reset_control_deassert(rst);
		msleep(50);
	}

	eth->tx_ring = dmam_alloc_coherent(dev, NUM_TX_DESC * sizeof(struct pdma_desc),
					   &eth->tx_phys, GFP_KERNEL);
	eth->rx_ring = dmam_alloc_coherent(dev, NUM_RX_DESC * sizeof(struct pdma_desc),
					   &eth->rx_phys, GFP_KERNEL);
	if (!eth->tx_ring || !eth->rx_ring)
		return -ENOMEM;

	mac_np = of_get_child_by_name(dev->of_node, "mac");
	err = of_get_ethdev_address(mac_np ?: dev->of_node, ndev);
	/* DSA finds its conduit by this node (the switch CPU port's "ethernet") */
	ndev->dev.of_node = mac_np ?: dev->of_node;
	if (err) {
		eth_hw_addr_random(ndev);
		dev_warn(dev, "using random MAC %pM\n", ndev->dev_addr);
	}

	ndev->netdev_ops = &mt7510_netdev_ops;
	/* Room for a VLAN tag and the 4-byte MTK switch tag in RX_BUF_LEN */
	ndev->max_mtu = RX_BUF_LEN - ETH_HLEN - VLAN_HLEN - ETH_FCS_LEN;
	netif_napi_add_weight(ndev, &eth->napi, mt7510_poll, NAPI_WEIGHT);

	err = devm_request_irq(dev, eth->irq, mt7510_isr, 0, dev_name(dev), eth);
	if (err)
		return err;

	platform_set_drvdata(pdev, eth);

	err = register_netdev(ndev);
	if (err) {
		netif_napi_del(&eth->napi);
		return err;
	}

	dev_info(dev, "MT7510 PDMA ethernet, irq %d, MAC %pM\n",
		 eth->irq, ndev->dev_addr);
	return 0;
}

static void mt7510_remove(struct platform_device *pdev)
{
	struct mt7510_eth *eth = platform_get_drvdata(pdev);

	unregister_netdev(eth->ndev);
	netif_napi_del(&eth->napi);
}

static const struct of_device_id mt7510_eth_match[] = {
	{ .compatible = "econet,mt7510-eth" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt7510_eth_match);

static struct platform_driver mt7510_eth_driver = {
	.probe	= mt7510_probe,
	.remove	= mt7510_remove,
	.driver	= {
		.name		= "mt7510-eth",
		.of_match_table	= mt7510_eth_match,
	},
};
module_platform_driver(mt7510_eth_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("EcoNet MT7510 PDMA ethernet driver");
