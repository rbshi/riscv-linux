#include "icenet.h"

static inline void post_send_frag(
		struct icenet_device *nic, skb_frag_t *frag, int last)
{
	uintptr_t addr = page_to_phys(frag->page.p) + frag->page_offset;
	uint64_t len = frag->size, partial = !last, packet;

	packet = (partial << 63) | (len << 48) | (addr & 0xffffffffffffL);
	iowrite64(packet, nic->iomem + ICENET_SEND_REQ);
}

void icenet_post_send(struct icenet_device *nic, struct sk_buff *skb)
{
	uintptr_t addr = virt_to_phys(skb->data);
	uint64_t len, partial, packet;
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int i;

	if (shinfo->nr_frags > 0) {
		len = skb_headlen(skb);
		partial = 1;
	} else {
		len = skb->len;
		partial = 0;
	}

	addr -= NET_IP_ALIGN;
	len += NET_IP_ALIGN;

	packet = (partial << 63) | (len << 48) | (addr & 0xffffffffffffL);
	iowrite64(packet, nic->iomem + ICENET_SEND_REQ);

	for (i = 0; i < shinfo->nr_frags; i++) {
		skb_frag_t *frag = &shinfo->frags[i];
		int last = i == (shinfo->nr_frags-1);
		post_send_frag(nic, frag, last);
	}

	sk_buff_cq_push(&nic->send_cq, skb);

//	printk(KERN_DEBUG "IceNet: tx addr=%lx len=%llu\n", addr, len);
}

void icenet_post_recv(struct icenet_device *nic, struct sk_buff *skb)
{
	int align = DMA_PTR_ALIGN(skb->data) - skb->data;
	uintptr_t addr;

	skb_reserve(skb, align);
	addr = virt_to_phys(skb->data);

	iowrite64(addr, nic->iomem + ICENET_RECV_REQ);
	sk_buff_cq_push(&nic->recv_cq, skb);
}

void icenet_complete_send(struct net_device *ndev)
{
	struct icenet_device *nic = netdev_priv(ndev);
	struct sk_buff *skb;
	int i, n, nsegs;

	n = send_comp_avail(nic);

	while (n > 0) {
		BUG_ON(SK_BUFF_CQ_COUNT(nic->send_cq) == 0);
		nsegs = sk_buff_cq_tail_nsegments(&nic->send_cq);

		if (nsegs > n)
			break;

		for (i = 0; i < nsegs; i++)
			ioread16(nic->iomem + ICENET_SEND_COMP);

		skb = sk_buff_cq_pop(&nic->send_cq);
		dev_consume_skb_any(skb);
		n -= nsegs;
	}
}

int icenet_complete_recv(struct net_device *ndev, int budget)
{
	struct icenet_device *nic = netdev_priv(ndev);
	struct sk_buff *skb;
	int len, i, n = recv_comp_avail(nic);

	if (n > budget)
		n = budget;

	for (i = 0; i < n; i++) {
		len = ioread16(nic->iomem + ICENET_RECV_COMP);
		skb = sk_buff_cq_pop(&nic->recv_cq);
		skb_put(skb, len);
		skb_pull(skb, NET_IP_ALIGN);

		skb->dev = ndev;
		skb->protocol = eth_type_trans(skb, ndev);
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len;
		netif_receive_skb(skb);

//		printk(KERN_DEBUG "IceNet: rx addr=%p, len=%d\n",
//				skb->data, len);
	}

	return n;
}

int icenet_alloc_recv(struct net_device *ndev, int n)
{
	struct icenet_device *nic = netdev_priv(ndev);
	int hw_recv_cnt = recv_req_avail(nic);
	int sw_recv_cnt = SK_BUFF_CQ_SPACE(nic->recv_cq);
	int i;

	if (hw_recv_cnt < n)
		n = hw_recv_cnt;

	if (sw_recv_cnt < n)
		n = sw_recv_cnt;

	for (i = 0; i < n; i++) {
		struct sk_buff *skb;
		skb = netdev_alloc_skb(ndev, MAX_FRAME_SIZE);
		icenet_post_recv(nic, skb);
	}

	return n;
}

void icenet_checksum_offload(struct icenet_device *nic, struct sk_buff *skb)
{
	void *start, *end;
	uint64_t i, len, partial, paddr, request;
	uint16_t counts, result;
	uint16_t *offset;
	struct skb_shared_info *shinfo = skb_shinfo(skb);

	start = skb_checksum_start(skb);

	if (shinfo->nr_frags > 0) {
		end = skb->data + skb_headlen(skb);
		partial = 1;
	} else {
		end = skb_end_pointer(skb);
		partial = 0;
	}

	len = (uint64_t) (end - start);
	paddr = virt_to_phys(start);
	BUG_ON(paddr >= (1L << 48));
	request = (partial << 63) | (len << 48) | paddr;

	do {
		counts = ioread16(nic->iomem + ICENET_CKSUM_COUNTS);
	} while ((counts & 0xff) < (shinfo->nr_frags + 1));

	iowrite64(request, nic->iomem + ICENET_CKSUM_REQ);

	for (i = 0; i < shinfo->nr_frags; i++) {
		skb_frag_t *frag = &shinfo->frags[i];
		partial = i < (shinfo->nr_frags - 1);
		len = frag->size;
		paddr = page_to_phys(frag->page.p) + frag->page_offset;
		BUG_ON(paddr >= (1L << 48));
		request = (partial << 63) | (len << 48) | paddr;
		iowrite64(request, nic->iomem + ICENET_CKSUM_REQ);
	}

	do {
		counts = ioread16(nic->iomem + ICENET_CKSUM_COUNTS);
	} while (((counts >> 8) & 0xff) == 0);

	result = ioread16(nic->iomem + ICENET_CKSUM_RESP);
	offset = start + skb->csum_offset;
	*offset = result;
	skb->ip_summed = CHECKSUM_NONE;
}
