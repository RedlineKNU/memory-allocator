// SPDX-License-Identifier: BSD-3-Clause

#include <unistd.h>
#include <sys/mman.h>

#include "osmem.h"
#include "internal.h"

static int heap_bootstrapped;

void *acquire_heap(size_t bytes)
{
	void *p = sbrk(bytes);

	DIE(p == (void *)-1, "sbrk");
	return p;
}

void *acquire_mapping(size_t payload)
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

void release_mapping(struct block_meta *b)
{
	int rc = munmap(b, META_SIZE + b->size);

	DIE(rc < 0, "munmap");
}

/*
 * One-shot 128 KiB brk() preallocation so small allocations don't
 * each pay for their own syscall. Safe to call repeatedly — only the
 * first call does work.
 */
struct block_meta *bootstrap_heap(void)
{
	if (heap_bootstrapped)
		return NULL;
	heap_bootstrapped = 1;

	struct block_meta *b = acquire_heap(MMAP_THRESHOLD);

	b->size = MMAP_THRESHOLD - META_SIZE;
	b->status = STATUS_FREE;
	heap_push_back(b);
	return b;
}
