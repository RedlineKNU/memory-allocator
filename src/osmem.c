// SPDX-License-Identifier: BSD-3-Clause

#include <string.h>

#include "osmem.h"
#include "internal.h"

#define PUBLIC __attribute__((visibility("default")))

PUBLIC void *os_malloc(size_t size);
PUBLIC void *os_calloc(size_t nmemb, size_t size);
PUBLIC void *os_realloc(void *ptr, size_t size);
PUBLIC void os_free(void *ptr);

/*
 * Satisfy `need` bytes from the brk-managed heap, growing it only when
 * best-fit fails. When growth is required, extending a free tail block
 * is preferred over appending a brand-new one (lab spec).
 */
static void *serve_from_heap(size_t need)
{
	bootstrap_heap();
	coalesce_free_runs();

	struct block_meta *b = best_fit(need);

	if (b) {
		b->status = STATUS_ALLOC;
		try_split(b, need);
		return block_payload(b);
	}

	struct block_meta *tail = heap_tail();

	if (tail && tail->status == STATUS_FREE) {
		acquire_heap(need - tail->size);
		tail->size = need;
		tail->status = STATUS_ALLOC;
		return block_payload(tail);
	}

	struct block_meta *fresh = acquire_heap(META_SIZE + need);

	fresh->size = need;
	fresh->status = STATUS_ALLOC;
	heap_push_back(fresh);
	return block_payload(fresh);
}

static void *serve_via_mmap(size_t need)
{
	struct block_meta *b = acquire_mapping(need);

	mmap_track(b);
	return block_payload(b);
}

/*
 * Enlarge heap block `b` without moving its payload:
 *   1. coalesce with following free blocks one-by-one;
 *   2. if `b` is the last block on the heap, extend via sbrk().
 * Returns 1 on success, 0 if the block must be relocated.
 */
static int try_grow_in_place(struct block_meta *b, size_t need)
{
	while (b->size < need && b->next && b->next->status == STATUS_FREE) {
		b->size += META_SIZE + b->next->size;
		b->next = b->next->next;
		if (b->next)
			b->next->prev = b;
	}

	if (b->size >= need)
		return 1;

	if (b == heap_tail()) {
		acquire_heap(need - b->size);
		b->size = need;
		return 1;
	}
	return 0;
}

void *os_malloc(size_t size)
{
	if (size == 0)
		return NULL;

	size_t need = align_payload(size);

	if (use_mmap_for(need, MMAP_THRESHOLD))
		return serve_via_mmap(need);
	return serve_from_heap(need);
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
		return NULL;

	size_t need = align_payload(nmemb * size);
	void *p;

	if (use_mmap_for(need, CALLOC_THRESHOLD))
		p = serve_via_mmap(need);
	else
		p = serve_from_heap(need);

	memset(p, 0, need);
	return p;
}

void os_free(void *ptr)
{
	if (!ptr)
		return;

	struct block_meta *b = block_from_payload(ptr);

	switch (b->status) {
	case STATUS_MAPPED:
		mmap_untrack(b);
		release_mapping(b);
		break;
	case STATUS_ALLOC:
		b->status = STATUS_FREE;
		break;
	default:
		break;
	}
}

void *os_realloc(void *ptr, size_t size)
{
	if (!ptr)
		return os_malloc(size);
	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	struct block_meta *b = block_from_payload(ptr);

	if (b->status == STATUS_FREE)
		return NULL;

	size_t need = align_payload(size);

	if (b->status == STATUS_MAPPED) {
		void *np = use_mmap_for(need, MMAP_THRESHOLD)
			? serve_via_mmap(need)
			: serve_from_heap(need);
		memcpy(np, ptr, b->size < need ? b->size : need);
		os_free(ptr);
		return np;
	}

	if (use_mmap_for(need, MMAP_THRESHOLD)) {
		void *np = serve_via_mmap(need);

		memcpy(np, ptr, b->size < need ? b->size : need);
		os_free(ptr);
		return np;
	}

	if (need <= b->size) {
		try_split(b, need);
		return ptr;
	}

	if (try_grow_in_place(b, need)) {
		try_split(b, need);
		return ptr;
	}

	/* Spec: already-coalesced blocks stay merged even if the new size still doesn't fit. */
	void *np = serve_from_heap(need);

	memcpy(np, ptr, b->size);
	b->status = STATUS_FREE;
	return np;
}
