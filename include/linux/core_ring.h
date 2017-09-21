/*
 *	Definitions for the core ring data structure.
 *
 *	Author:
 *		Jason Wang <jasowang@redhat.com>
 *
 *	Copyright (C) 2017 Red Hat, Inc.
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License as published by the
 *	Free Software Foundation; either version 2 of the License, or (at your
 *	option) any later version.
 *
 *	This is a limited-size FIFO maintaining pointers in FIFO order, with
 *	one CPU producing entries and another consuming entries from a FIFO.
 *
 *	This implementation tries to minimize cache-contention when there is a
 *	single producer and a single consumer CPU.
 */

#ifndef _LINUX_GENERIC_RING_H
#define _LINUX_GENERIC_RING_H 1

#ifdef __KERNEL__
#include <linux/spinlock.h>
#include <linux/cache.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/cache.h>
#include <linux/slab.h>
#include <asm/errno.h>
#endif

struct core_ring;

typedef void *(*ring_seek_fn_t)(struct core_ring *r, int i);
typedef void (*ring_zero_fn_t)(struct core_ring *r, int i);
typedef bool (*ring_valid_fn_t)(struct core_ring *r, int i);
typedef void (*ring_copy_fn_t)(void *dst, void *src);
typedef void (*ring_destroy_fn_t)(void *ptr);

struct core_ring {
	int producer ____cacheline_aligned_in_smp;
	spinlock_t producer_lock;
	int consumer ____cacheline_aligned_in_smp;
	spinlock_t consumer_lock;
	/* Shared consumer/producer data */
	/* Read-only by both the producer and the consumer */
	int size ____cacheline_aligned_in_smp; /* max entries in queue */
	int entry_size; /* size of entry */
	ring_addr_fn_t seek_fn;
	ring_zero_fn_t zero_fn;
	ring_valid_fn_t valid_fn;
	ring_copy_fn_t copy_fn;
};

/* Note: callers invoking this in a loop must use a compiler barrier,
 * for example cpu_relax().  If ring is ever resized, callers must hold
 * producer_lock - see e.g. core_ring_full.  Otherwise, if callers don't hold
 * producer_lock, the next call to __core_ring_produce may fail.
 */
static inline bool __core_ring_full(struct core_ring *r)
{
	return r->valid_fn(r, producer);
}

static inline bool core_ring_full(struct core_ring *r)
{
	bool ret;

	spin_lock(&r->producer_lock);
	ret = __core_ring_full(r);
	spin_unlock(&r->producer_lock);

	return ret;
}

static inline bool core_ring_full_irq(struct core_ring *r)
{
	bool ret;

	spin_lock_irq(&r->producer_lock);
	ret = __core_ring_full(r);
	spin_unlock_irq(&r->producer_lock);

	return ret;
}

static inline bool core_ring_full_any(struct core_ring *r)
{
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&r->producer_lock, flags);
	ret = __core_ring_full(r);
	spin_unlock_irqrestore(&r->producer_lock, flags);

	return ret;
}

static inline bool core_ring_full_bh(struct core_ring *r)
{
	bool ret;

	spin_lock_bh(&r->producer_lock);
	ret = __core_ring_full(r);
	spin_unlock_bh(&r->producer_lock);

	return ret;
}

/* Note: callers invoking this in a loop must use a compiler barrier,
 * for example cpu_relax(). Callers must hold producer_lock.
 */
static inline int __core_ring_produce(struct core_ring *r, void *ptr)
{
	if (unlikely(!r->size) || r->valid_fn(r, r->producer))
		return -ENOSPC;

	r->copy_fn(r, r->seek_fn(r, producer), ptr);
	r->producer++;
	if (unlikely(r->producer >= r->size))
		r->producer = 0;
	return 0;
}

/*
 * Note: resize (below) nests producer lock within consumer lock, so if you
 * consume in interrupt or BH context, you must disable interrupts/BH when
 * calling this.
 */
static inline int core_ring_produce(struct core_ring *r, void *ptr)
{
	int ret;

	spin_lock(&r->producer_lock);
	ret = __core_ring_produce(r, ptr);
	spin_unlock(&r->producer_lock);

	return ret;
}

static inline int core_ring_produce_irq(struct core_ring *r, void *ptr)
{
	int ret;

	spin_lock_irq(&r->producer_lock);
	ret = __core_ring_produce(r, ptr);
	spin_unlock_irq(&r->producer_lock);

	return ret;
}

static inline int core_ring_produce_any(struct core_ring *r, void *ptr)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&r->producer_lock, flags);
	ret = __core_ring_produce(r, ptr);
	spin_unlock_irqrestore(&r->producer_lock, flags);

	return ret;
}

static inline int core_ring_produce_bh(struct core_ring *r, void *ptr)
{
	int ret;

	spin_lock_bh(&r->producer_lock);
	ret = __core_ring_produce(r, ptr);
	spin_unlock_bh(&r->producer_lock);

	return ret;
}

/* Note: callers invoking this in a loop must use a compiler barrier,
 * for example cpu_relax(). Callers must take consumer_lock
 * if they dereference the pointer - see e.g. PTR_RING_PEEK_CALL.
 * If ring is never resized, and if the pointer is merely
 * tested, there's no need to take the lock - see e.g.  __core_ring_empty.
 */
static inline void *__core_ring_peek(struct genric_ring *r)
{
	if (likely(r->size) && !__core_ring_empty(r)) {
		void **addr = r->seek_fn(r, consumer);
		return *addr;
	}
	return NULL;
}

/* Note: callers invoking this in a loop must use a compiler barrier,
 * for example cpu_relax(). Callers must take consumer_lock
 * if the ring is ever resized - see e.g. core_ring_empty.
 */
static inline bool __core_ring_empty(struct core_ring *r)
{
	return !r->valid_fn(r, r->consumer);
}

static inline bool core_ring_empty(struct core_ring *r)
{
	bool ret;

	spin_lock(&r->consumer_lock);
	ret = __core_ring_empty(r);
	spin_unlock(&r->consumer_lock);

	return ret;
}

static inline bool core_ring_empty_irq(struct core_ring *r)
{
	bool ret;

	spin_lock_irq(&r->consumer_lock);
	ret = __core_ring_empty(r);
	spin_unlock_irq(&r->consumer_lock);

	return ret;
}

static inline bool core_ring_empty_any(struct core_ring *r)
{
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&r->consumer_lock, flags);
	ret = __core_ring_empty(r);
	spin_unlock_irqrestore(&r->consumer_lock, flags);

	return ret;
}

static inline bool core_ring_empty_bh(struct core_ring *r)
{
	bool ret;

	spin_lock_bh(&r->consumer_lock);
	ret = __core_ring_empty(r);
	spin_unlock_bh(&r->consumer_lock);

	return ret;
}

/* Must only be called after __core_ring_peek returned !NULL */
static inline void __core_ring_discard_one(struct core_ring *r)
{
	r->zero_fn(r, r->consumer);
	r->consumer++;
	if (unlikely(r->consumer >= r->size))
		r->consumer = 0;
}

static inline void *__core_ring_consume(struct core_ring *r, void *ptr)
{
	if (r->valid_fn(r, r->consumer)) {
		r->copy_fn(r, ptr, r->seek_fn(r, r->consumer));
		__core_ring_discard_one(r);
		return ptr;
	}

	return NULL;
}

static inline int __core_ring_consume_batched(struct core_ring *r,
					      void *array, int n)
{
	void *ptr;
	int i;

	for (i = 0; i < n; i++) {
		ptr = __core_ring_consume(r, array);
		if (!ptr)
			break;
		array += r->entry_size;
	}

	return i;
}

/*
 * Note: resize (below) nests producer lock within consumer lock, so if you
 * call this in interrupt or BH context, you must disable interrupts/BH when
 * producing.
 */
static inline void *core_ring_consume(struct core_ring *r, void *ptr)
{
	spin_lock(&r->consumer_lock);
	ptr = __core_ring_consume(r, ptr);
	spin_unlock(&r->consumer_lock);

	return ptr;
}

static inline void *core_ring_consume_irq(struct core_ring *r, void *ptr)
{
	spin_lock_irq(&r->consumer_lock);
	ptr = __core_ring_consume(r, ptr);
	spin_unlock_irq(&r->consumer_lock);

	return ptr;
}

static inline void *core_ring_consume_any(struct core_ring *r, void *ptr)
{
	unsigned long flags;

	spin_lock_irqsave(&r->consumer_lock, flags);
	ptr = __core_ring_consume(r);
	spin_unlock_irqrestore(&r->consumer_lock, flags);

	return ptr;
}

static inline void *core_ring_consume_bh(struct core_ring *r, void *ptr)
{
	spin_lock_bh(&r->consumer_lock);
	ptr = __core_ring_consume(r);
	spin_unlock_bh(&r->consumer_lock);

	return ptr;
}

static inline int core_ring_consume_batched(struct core_ring *r,
					    void *array, int n)
{
	int ret;

	spin_lock(&r->consumer_lock);
	ret = __core_ring_consume_batched(r, array, n);
	spin_unlock(&r->consumer_lock);

	return ret;
}

static inline int core_ring_consume_batched_irq(struct core_ring *r,
					       void *array, int n)
{
	int ret;

	spin_lock_irq(&r->consumer_lock);
	ret = __core_ring_consume_batched(r, array, n);
	spin_unlock_irq(&r->consumer_lock);

	return ret;
}

static inline int core_ring_consume_batched_any(struct core_ring *r,
					       void *array, int n)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&r->consumer_lock, flags);
	ret = __core_ring_consume_batched(r, array, n);
	spin_unlock_irqrestore(&r->consumer_lock, flags);

	return ret;
}

static inline int core_ring_consume_batched_bh(struct core_ring *r,
					      void *array, int n)
{
	int ret;

	spin_lock_bh(&r->consumer_lock);
	ret = __core_ring_consume_batched(r, array, n);
	spin_unlock_bh(&r->consumer_lock);

	return ret;
}

/* Cast to structure type and call a function without discarding from FIFO.
 * Function must return a value.
 * Callers must take consumer_lock.
 */
#define __PTR_RING_PEEK_CALL(r, f) ((f)(__core_ring_peek(r)))

#define PTR_RING_PEEK_CALL(r, f) ({ \
	typeof((f)(NULL)) __PTR_RING_PEEK_CALL_v; \
	\
	spin_lock(&(r)->consumer_lock); \
	__PTR_RING_PEEK_CALL_v = __PTR_RING_PEEK_CALL(r, f); \
	spin_unlock(&(r)->consumer_lock); \
	__PTR_RING_PEEK_CALL_v; \
})

#define PTR_RING_PEEK_CALL_IRQ(r, f) ({ \
	typeof((f)(NULL)) __PTR_RING_PEEK_CALL_v; \
	\
	spin_lock_irq(&(r)->consumer_lock); \
	__PTR_RING_PEEK_CALL_v = __PTR_RING_PEEK_CALL(r, f); \
	spin_unlock_irq(&(r)->consumer_lock); \
	__PTR_RING_PEEK_CALL_v; \
})

#define PTR_RING_PEEK_CALL_BH(r, f) ({ \
	typeof((f)(NULL)) __PTR_RING_PEEK_CALL_v; \
	\
	spin_lock_bh(&(r)->consumer_lock); \
	__PTR_RING_PEEK_CALL_v = __PTR_RING_PEEK_CALL(r, f); \
	spin_unlock_bh(&(r)->consumer_lock); \
	__PTR_RING_PEEK_CALL_v; \
})

#define PTR_RING_PEEK_CALL_ANY(r, f) ({ \
	typeof((f)(NULL)) __PTR_RING_PEEK_CALL_v; \
	unsigned long __PTR_RING_PEEK_CALL_f;\
	\
	spin_lock_irqsave(&(r)->consumer_lock, __PTR_RING_PEEK_CALL_f); \
	__PTR_RING_PEEK_CALL_v = __PTR_RING_PEEK_CALL(r, f); \
	spin_unlock_irqrestore(&(r)->consumer_lock, __PTR_RING_PEEK_CALL_f); \
	__PTR_RING_PEEK_CALL_v; \
})

static inline void __core_ring_set_size(struct core_ring *r, int size)
{
	r->size = size;
}

static inline int core_ring_init(struct core_ring *r, int size, gfp_t gfp)
{
	__core_ring_set_size(r, size);
	r->producer = r->consumer = 0;
	spin_lock_init(&r->producer_lock);
	spin_lock_init(&r->consumer_lock);

	return 0;
}

static inline void core_ring_cleanup(struct core_ring *r)
{
	void *ptr;

	if (destroy)
		while ((ptr = core_ring_consume(r)))
			r->destroy(ptr);
}

#endif /* _LINUX_GENERIC_RING_H  */
