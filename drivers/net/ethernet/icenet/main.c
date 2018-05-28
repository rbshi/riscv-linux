#include <linux/errno.h>
#include <linux/types.h>

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>

#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/of_irq.h>

#include "icenet.h"

static irqreturn_t icenet_tx_isr(int irq, void *data)
{
	struct net_device *ndev = data;
	struct icenet_device *nic = netdev_priv(ndev);
	int space;

	if (irq != nic->tx_irq)
		return IRQ_NONE;

	spin_lock(&nic->tx_lock);

	icenet_complete_send(ndev);
	space = send_space(nic);

	if (space >= CONFIG_ICENET_TX_INTERRUPT_THRESHOLD)
		clear_intmask(nic, ICENET_INTMASK_TX);

	if (space > 0 && netif_queue_stopped(ndev))
		netif_wake_queue(ndev);

	spin_unlock(&nic->tx_lock);

	return IRQ_HANDLED;
}

static irqreturn_t icenet_rx_isr(int irq, void *data)
{
	struct net_device *ndev = data;
	struct icenet_device *nic = netdev_priv(ndev);

	if (irq != nic->rx_irq)
		return IRQ_NONE;

	clear_intmask(nic, ICENET_INTMASK_RX);

	if (likely(napi_schedule_prep(&nic->napi)))
		__napi_schedule(&nic->napi);

	return IRQ_HANDLED;
}

static int icenet_rx_poll(struct napi_struct *napi, int budget)
{
	struct icenet_device *nic;
	struct net_device *ndev;
	int completed, allocated;

	nic = container_of(napi, struct icenet_device, napi);
	ndev = dev_get_drvdata(nic->dev);

	spin_lock(&nic->rx_lock);

	completed = icenet_complete_recv(ndev, budget);
	allocated = icenet_alloc_recv(ndev, completed);

	spin_unlock(&nic->rx_lock);

	BUG_ON(allocated != completed);

	if (completed < budget) {
		napi_complete(napi);
		set_intmask(nic, ICENET_INTMASK_RX);
	}

	return completed;
}

static int icenet_parse_addr(struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);
	struct device *dev = nic->dev;
	struct device_node *node = dev->of_node;
	struct resource regs;
	int err;

	err = of_address_to_resource(node, 0, &regs);
	if (err) {
		dev_err(dev, "missing \"reg\" property\n");
		return err;
	}

	nic->iomem = devm_ioremap_resource(dev, &regs);
	if (IS_ERR(nic->iomem)) {
		dev_err(dev, "could not remap io address %llx", regs.start);
		return PTR_ERR(nic->iomem);
	}

	return 0;
}

static int icenet_parse_irq(struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);
	struct device *dev = nic->dev;
	struct device_node *node = dev->of_node;
	int err;

	nic->tx_irq = irq_of_parse_and_map(node, 0);
	err = devm_request_irq(dev, nic->tx_irq, icenet_tx_isr,
			IRQF_SHARED | IRQF_NO_THREAD,
			ICENET_NAME, ndev);
	if (err) {
		dev_err(dev, "could not obtain TX irq %d\n", nic->tx_irq);
		return err;
	}

	nic->rx_irq = irq_of_parse_and_map(node, 1);
	err = devm_request_irq(dev, nic->rx_irq, icenet_rx_isr,
			IRQF_SHARED | IRQF_NO_THREAD,
			ICENET_NAME, ndev);
	if (err) {
		dev_err(dev, "could not obtain RX irq %d\n", nic->rx_irq);
		return err;
	}

	return 0;
}

static int icenet_open(struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);
	unsigned long flags;

	spin_lock_irqsave(&nic->rx_lock, flags);
	icenet_alloc_recv(ndev, CONFIG_ICENET_RING_SIZE);
	spin_unlock_irqrestore(&nic->rx_lock, flags);

	netif_start_queue(ndev);
	napi_enable(&nic->napi);
	set_intmask(nic, ICENET_INTMASK_RX);

	printk(KERN_DEBUG "IceNet: opened device\n");

	return 0;
}

static int icenet_stop(struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);

	clear_intmask(nic, ICENET_INTMASK_BOTH);
	napi_synchronize(&nic->napi);
	napi_disable(&nic->napi);
	netif_stop_queue(ndev);

	printk(KERN_DEBUG "IceNet: stopped device\n");
	return 0;
}

static int icenet_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);
	unsigned long flags;
	int space, nsegs = skb_shinfo(skb)->nr_frags + 1;
	int tx_err = 0;

	spin_lock_irqsave(&nic->tx_lock, flags);

	space = send_space(nic);

	if (space < CONFIG_ICENET_TX_INTERRUPT_THRESHOLD)
		set_intmask(nic, ICENET_INTMASK_TX);

	if (space < nsegs) {
		// Try to clear space first
		icenet_complete_send(ndev);
		space = send_space(nic);
		// If we couldn't reclaim enough space, run the error handler
		tx_err = space < nsegs;
	}

	if (tx_err) {
		printk(KERN_WARNING "Not enough space in TX ring\n");
		netif_stop_queue(ndev);
		set_intmask(nic, ICENET_INTMASK_TX);
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		spin_unlock_irqrestore(&nic->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}

	if (skb->ip_summed == CHECKSUM_PARTIAL)
		icenet_checksum_offload(nic, skb);

	skb_tx_timestamp(skb);
	icenet_post_send(nic, skb);
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;

	spin_unlock_irqrestore(&nic->tx_lock, flags);

	return NETDEV_TX_OK;
}

static void icenet_init_mac_address(struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);
	uint64_t macaddr = ioread64(nic->iomem + ICENET_MACADDR);

	ndev->addr_assign_type = NET_ADDR_PERM;
	memcpy(ndev->dev_addr, &macaddr, MACADDR_BYTES);

	if (!is_valid_ether_addr(ndev->dev_addr))
		printk(KERN_WARNING "Invalid MAC address\n");
}

static const struct net_device_ops icenet_ops = {
	.ndo_open = icenet_open,
	.ndo_stop = icenet_stop,
	.ndo_start_xmit = icenet_start_xmit
};

static int icenet_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev;
	struct icenet_device *nic;
	int ret;

	if (!dev->of_node)
		return -ENODEV;

	ndev = devm_alloc_etherdev(dev, sizeof(struct icenet_device));
	if (!ndev)
		return -ENOMEM;

	dev_set_drvdata(dev, ndev);
	nic = netdev_priv(ndev);
	nic->dev = dev;

	netif_napi_add(ndev, &nic->napi, icenet_rx_poll, CONFIG_ICENET_RX_BUDGET);

	ether_setup(ndev);
	ndev->flags &= ~IFF_MULTICAST;
	ndev->netdev_ops = &icenet_ops;
	ndev->hw_features = (NETIF_F_RXCSUM | NETIF_F_HW_CSUM | NETIF_F_SG);
	ndev->features = ndev->hw_features;
	ndev->vlan_features = ndev->hw_features;

	spin_lock_init(&nic->tx_lock);
	spin_lock_init(&nic->rx_lock);
	sk_buff_cq_init(&nic->send_cq);
	sk_buff_cq_init(&nic->recv_cq);

	if ((ret = icenet_parse_addr(ndev)) < 0)
		return ret;

	icenet_init_mac_address(ndev);
	if ((ret = register_netdev(ndev)) < 0) {
		dev_err(dev, "Failed to register netdev\n");
		return ret;
	}

	if ((ret = icenet_parse_irq(ndev)) < 0)
		return ret;

	printk(KERN_INFO "Registered IceNet NIC %02x:%02x:%02x:%02x:%02x:%02x: "
			 "%d send queue size, "
			 "%d recv queue size\n",
		ndev->dev_addr[0],
		ndev->dev_addr[1],
		ndev->dev_addr[2],
		ndev->dev_addr[3],
		ndev->dev_addr[4],
		ndev->dev_addr[5],
		send_req_avail(nic),
		recv_req_avail(nic));

	return 0;
}

static int icenet_remove(struct platform_device *pdev)
{
	struct net_device *ndev;
	struct icenet_device *nic;

	ndev = platform_get_drvdata(pdev);
	nic = netdev_priv(ndev);

	netif_napi_del(&nic->napi);
	unregister_netdev(ndev);

	return 0;
}

static struct of_device_id icenet_of_match[] = {
	{ .compatible = "ucbbar,ice-nic" },
	{}
};

static struct platform_driver icenet_driver = {
	.driver = {
		.name = ICENET_NAME,
		.of_match_table = icenet_of_match,
		.suppress_bind_attrs = true
	},
	.probe = icenet_probe,
	.remove = icenet_remove
};

builtin_platform_driver(icenet_driver);
