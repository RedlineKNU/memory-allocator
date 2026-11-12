// SPDX-License-Identifier: BSD-3-Clause

#include "internal.h"

struct block_meta *heap_list;
struct block_meta *mmap_list;

void *block_payload(struct block_meta *b)
{
	return (char *)b + META_SIZE;
}

struct block_meta *block_from_payload(void *payload)
{
	return (struct block_meta *)((char *)payload - META_SIZE);
}

struct block_meta *heap_tail(void)
{
	struct block_meta *cur = heap_list;

	if (!cur)
		return NULL;
	while (cur->next)
		cur = cur->next;
	return cur;
}

void heap_push_back(struct block_meta *b)
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

void mmap_track(struct block_meta *b)
{
	b->prev = NULL;
	b->next = mmap_list;
	if (mmap_list)
		mmap_list->prev = b;
	mmap_list = b;
}

void mmap_untrack(struct block_meta *b)
{
	if (b->prev)
		b->prev->next = b->next;
	else
		mmap_list = b->next;
	if (b->next)
		b->next->prev = b->prev;
}

/* Merge every run of adjacent STATUS_FREE blocks into one. */
void coalesce_free_runs(void)
{
	struct block_meta *b = heap_list;

	while (b && b->next) {
		if (b->status == STATUS_FREE && b->next->status == STATUS_FREE) {
			b->size += META_SIZE + b->next->size;
			b->next = b->next->next;
			if (b->next)
				b->next->prev = b;
			/* don't advance — the new neighbour may also be free */
			continue;
		}
		b = b->next;
	}
}

/* Smallest STATUS_FREE block that can hold `need` bytes of payload. */
struct block_meta *best_fit(size_t need)
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

/*
 * Shrink `b` to `need` bytes and publish the leftover as a free block.
 * Does nothing if the leftover is too small to form a usable block.
 */
int try_split(struct block_meta *b, size_t need)
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
