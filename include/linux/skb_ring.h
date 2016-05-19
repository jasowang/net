/*
 *	skb ring implemented with circular buffer.
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#ifndef _LINUX_SKB_RING_H
#define _LINUX_SKB_RING_H

#include <asm/barrier.h>
#include <linux/spinlock.h>
#include <linux/circ_buf.h>
#include <linux/skbuff.h>
#include <linux/compiler.h>

struct skb_desc {
	struct sk_buff *skb;
	int len; /* Cached skb len for peeking */
};

struct skb_ring {
	/* reader lock */
	spinlock_t rlock;
	unsigned long tail ____cacheline_aligned_in_smp;
	unsigned long size ____cacheline_aligned_in_smp;
	struct skb_desc *descs;
	unsigned long head ____cacheline_aligned_in_smp;
        /* writer lock */;
	spinlock_t wlock;
};

int skb_ring_init(struct skb_ring *ring, unsigned long size);
void skb_ring_purge(struct skb_ring *ring);
int skb_ring_queue(struct skb_ring *ring, struct sk_buff *skb);
struct sk_buff *skb_ring_dequeue(struct skb_ring *ring);

int skb_ring_empty(struct skb_ring *ring)
{
	return ACCESS_ONCE(ring->head) == ACCESS_ONCE(ring->tail);
}

int skb_ring_peek(struct skb_ring *ring)
{
	unsigned long head = smp_load_acquire(&ring->head);
	unsigned long tail = ACCESS_ONCE(ring->tail);
	int ret = 0;

	if (CIRC_CNT(head, tail, ring->size) >= 1)
		ret = ring->descs[tail].len;

	return ret;
}

int skb_ring_queue_len(struct skb_ring *ring)
{
	unsigned long head = ACCESS_ONCE(ring->head);
	unsigned long tail = ACCESS_ONCE(ring->tail);

	return CIRC_CNT(head, tail, ring->size);
}

#endif /* _LINUX_SKB_RING_H */
