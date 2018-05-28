#ifndef __ICENET_H__
#define __ICENET_H__

#define ICENET_NAME "icenet"
#define ICENET_SEND_REQ 0
#define ICENET_RECV_REQ 8
#define ICENET_SEND_COMP 16
#define ICENET_RECV_COMP 18
#define ICENET_COUNTS 20
#define ICENET_MACADDR 24
#define ICENET_INTMASK 32
#define ICENET_CKSUM_COUNTS 36
#define ICENET_CKSUM_RESP 38
#define ICENET_CKSUM_REQ 40

#define ICENET_INTMASK_TX 1
#define ICENET_INTMASK_RX 2
#define ICENET_INTMASK_BOTH 3

#define ALIGN_BYTES 8
#define ALIGN_MASK 0x7
#define ALIGN_SHIFT 3
#define MAX_FRAME_SIZE (190 * ALIGN_BYTES)
#define DMA_PTR_ALIGN(p) ((typeof(p)) (__ALIGN_KERNEL((uintptr_t) (p), ALIGN_BYTES)))
#define DMA_LEN_ALIGN(n) (((((n) - 1) >> ALIGN_SHIFT) + 1) << ALIGN_SHIFT)
#define MACADDR_BYTES 6

#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#include "skbuff_cq.h"

struct icenet_device {
	struct device *dev;
	void __iomem *iomem;
	struct napi_struct napi;
	struct sk_buff_cq send_cq;
	struct sk_buff_cq recv_cq;
	spinlock_t tx_lock;
	spinlock_t rx_lock;
	int tx_irq;
	int rx_irq;
};

static inline int send_req_avail(struct icenet_device *nic)
{
	return ioread32(nic->iomem + ICENET_COUNTS) & 0xff;
}

static inline int recv_req_avail(struct icenet_device *nic)
{
	return (ioread32(nic->iomem + ICENET_COUNTS) >> 8) & 0xff;
}

static inline int send_comp_avail(struct icenet_device *nic)
{
	return (ioread32(nic->iomem + ICENET_COUNTS) >> 16) & 0xff;
}

static inline int recv_comp_avail(struct icenet_device *nic)
{
	return (ioread32(nic->iomem + ICENET_COUNTS) >> 24) & 0xff;
}

static inline void set_intmask(struct icenet_device *nic, uint32_t mask)
{
	atomic_t *mem = nic->iomem + ICENET_INTMASK;
	atomic_fetch_or(mask, mem);
}

static inline void clear_intmask(struct icenet_device *nic, uint32_t mask)
{
	atomic_t *mem = nic->iomem + ICENET_INTMASK;
	atomic_fetch_and(~mask, mem);
}

static inline int send_space(struct icenet_device *nic)
{
	int avail = send_req_avail(nic);
	int space = SK_BUFF_CQ_SPACE(nic->send_cq);

	return (avail < space) ? avail : space;
}

void icenet_post_send(struct icenet_device *nic, struct sk_buff *skb);
void icenet_post_recv(struct icenet_device *nic, struct sk_buff *skb);
void icenet_complete_send(struct net_device *ndev);
int  icenet_complete_recv(struct net_device *ndev, int budget);
int  icenet_alloc_recv(struct net_device *ndev, int n);
void icenet_checksum_offload(struct icenet_device *nic, struct sk_buff *skb);

#endif
