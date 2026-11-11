// SPDX-License-Identifier: BSD-3-Clause

#include <unistd.h>
#include <sys/mman.h>
#include <string.h>

#include "osmem.h"
#include "block_meta.h"

#define ALIGNMENT		((size_t)8)
#define ALIGN_UP(x)		(((x) + ALIGNMENT - 1) & ~(ALIGNMENT - 1))
#define META_SIZE		ALIGN_UP(sizeof(struct block_meta))
#define MMAP_THRESHOLD		((size_t)(128 * 1024))
#define CALLOC_THRESHOLD	((size_t)4096)
#define MIN_SPLIT_SIZE		(META_SIZE + ALIGNMENT)

static struct block_meta *heap_list;
static struct block_meta *mmap_list;
static int heap_bootstrapped;

static void *block_payload(struct block_meta *b)
{
	return (char *)b + META_SIZE;
}

static struct block_meta *block_from_payload(void *p)
{
	return (struct block_meta *)((char *)p - META_SIZE);
}

static struct block_meta *heap_tail(void)
{
	struct block_meta *cur = heap_list;

	if (!cur)
		return NULL;
	while (cur->next)
		cur = cur->next;
	return cur;
}

static void heap_push_back(struct block_meta *b)
{
	struct block_meta *tail = heap_tail();

	b->next = NULL;
	if (!tail) {
		b->prev = NULL;
		heap_list = b;
		return;
	}
	tail->next = b;
	b->prev = tail;
}

static void mmap_track(struct block_meta *b)
{
	b->prev = NULL;
	b->next = mmap_list;
	if (mmap_list)
		mmap_list->prev = b;
	mmap_list = b;
}

static void mmap_untrack(struct block_meta *b)
{
	if (b->prev)
		b->prev->next = b->next;
	else
		mmap_list = b->next;
	if (b->next)
		b->next->prev = b->prev;
}

static void coalesce_free_runs(void)
{
	struct block_meta *b = heap_list;

	while (b && b->next) {
		if (b->status == STATUS_FREE && b->next->status == STATUS_FREE) {
			b->size += META_SIZE + b->next->size;
			b->next = b->next->next;
			if (b->next)
				b->next->prev = b;
			continue;
		}
		b = b->next;
	}
}

static struct block_meta *best_fit(size_t need)
{
	struct block_meta *winner = NULL;

	for (struct block_meta *b = heap_list; b; b = b->next) {
		if (b->status != STATUS_FREE || b->size < need)
			continue;
		if (!winner || b->size < winner->size)
			winner = b;
	}
	return winner;
}

static int try_split(struct block_meta *b, size_t need)
{
	if (b->size < need + MIN_SPLIT_SIZE)
		return 0;

	struct block_meta *tail = (struct block_meta *)((char *)b + META_SIZE + need);

	tail->size = b->size - need - META_SIZE;
	tail->status = STATUS_FREE;
	tail->prev = b;
	tail->next = b->next;

	if (b->next)
		b->next->prev = tail;
	b->next = tail;
	b->size = need;
	return 1;
}

static void *acquire_heap(size_t bytes)
{
	void *p = sbrk(bytes);

	DIE(p == (void *)-1, "sbrk");
	return p;
}

static struct block_meta *acquire_mapping(size_t payload)
{
	size_t len = META_SIZE + payload;
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	DIE(p == MAP_FAILED, "mmap");

	struct block_meta *b = (struct block_meta *)p;

	b->size = payload;
	b->status = STATUS_MAPPED;
	return b;
}

static void release_mapping(struct block_meta *b)
{
	int rc = munmap(b, META_SIZE + b->size);

	DIE(rc < 0, "munmap");
}

static void bootstrap_heap(void)
{
	if (heap_bootstrapped)
		return;
	heap_bootstrapped = 1;

	struct block_meta *b = acquire_heap(MMAP_THRESHOLD);

	b->size = MMAP_THRESHOLD - META_SIZE;
	b->status = STATUS_FREE;
	heap_push_back(b);
}

static int use_mmap_for(size_t aligned_payload, size_t threshold)
{
	return aligned_payload + META_SIZE >= threshold;
}

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

	size_t need = ALIGN_UP(size);

	if (use_mmap_for(need, MMAP_THRESHOLD))
		return serve_via_mmap(need);
	return serve_from_heap(need);
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
		return NULL;

	size_t need = ALIGN_UP(nmemb * size);
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

	size_t need = ALIGN_UP(size);

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
