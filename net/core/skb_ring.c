/*
 */

#include <linux/skb_ring.h>
#include <linux/if_vlan.h>

int skb_ring_init(struct skb_ring *ring, unsigned long size)
{
	spin_lock_init(&ring->rlock);
	spin_lock_init(&ring->wlock);

	ring->head = 0;
	ring->tail = 0;

	ring->descs = kmalloc(size * sizeof *ring->descs, GFP_ATOMIC);
	if (!ring->descs)
		return -ENOMEM;

	ring->size = size;
	/* FIXEME: check power of 2 */

	return 0;
}
EXPORT_SYMBOL(skb_ring_init);

void skb_ring_purge(struct skb_ring *ring)
{
	unsigned long head, tail;

	spin_lock_bh(&ring->rlock);
	spin_lock(&ring->wlock);

	head = smp_load_acquire(&ring->head);
	tail = ring->tail;

	while (CIRC_CNT(head, tail, ring->size) >= 1) {
		struct skb_desc *desc = &ring->descs[tail];
		struct sk_buff *skb = desc->skb;
		kfree_skb(skb);
		/* read descriptor before incrementing tail. */
		smp_store_release(&ring->tail, (tail + 1) & (ring->size - 1));
	}

	spin_unlock(&ring->wlock);
	spin_unlock_bh(&ring->rlock);
}
EXPORT_SYMBOL(skb_ring_purge);

int skb_ring_queue(struct skb_ring *ring, struct sk_buff *skb)
{
	unsigned long head, tail;
	int ret = 0;

	spin_lock(&ring->wlock);

	tail = smp_load_acquire(&ring->tail);
	head = ring->head;

	if (CIRC_SPACE(head, tail, ring->size) >= 1) {
		struct skb_desc *desc = &ring->descs[head];

		desc->skb = skb;
		desc->len = skb->len;
		if (skb_vlan_tag_present(skb))
			desc->len += VLAN_HLEN;

		/* produce descriptor before incrementing head. */
		smp_store_release(&ring->head, (head + 1) & (ring->size - 1));
	} else {
		ret = -EFAULT;
	}

	spin_unlock(&ring->wlock);

	return ret;
}
EXPORT_SYMBOL(skb_ring_queue);


struct sk_buff *skb_ring_dequeue(struct skb_ring *ring)
{
	unsigned long head, tail;
	struct sk_buff *skb = NULL;
	struct skb_desc *desc;

	spin_lock(&ring->rlock);
	/* Read index before reading contents at that index. */
	head = smp_load_acquire(&ring->head);
	tail = ring->tail;

	if (CIRC_CNT(head, tail, ring->size) >= 1) {
		desc = &ring->descs[tail];
		skb = desc->skb;
		/* read descriptor before incrementing tail. */
		smp_store_release(&ring->tail, (tail + 1) & (ring->size - 1));
	}

	spin_unlock(&ring->rlock);

	return skb;
}
EXPORT_SYMBOL(skb_ring_dequeue);

