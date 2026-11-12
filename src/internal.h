/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <stddef.h>
#include "block_meta.h"

#define ALIGNMENT		((size_t)8)
#define ALIGN_UP(x)		(((x) + ALIGNMENT - 1) & ~(ALIGNMENT - 1))
#define META_SIZE		ALIGN_UP(sizeof(struct block_meta))

/* Payload >= MMAP_THRESHOLD goes through mmap (malloc/realloc path). */
#define MMAP_THRESHOLD		((size_t)(128 * 1024))
/* calloc crosses into mmap at one page — getpagesize() on x86_64 Linux. */
#define CALLOC_THRESHOLD	((size_t)4096)
/* Minimum size the remainder must have to be worth splitting off. */
#define MIN_SPLIT_SIZE		(META_SIZE + ALIGNMENT)

/* Two independent lists: brk-managed blocks vs. one-off mmap'd blocks. */
extern struct block_meta *heap_list;
extern struct block_meta *mmap_list;

void *block_payload(struct block_meta *b);
struct block_meta *block_from_payload(void *payload);

struct block_meta *heap_tail(void);
void heap_push_back(struct block_meta *b);

void mmap_track(struct block_meta *b);
void mmap_untrack(struct block_meta *b);

void coalesce_free_runs(void);
struct block_meta *best_fit(size_t need);
int try_split(struct block_meta *b, size_t need);

void *acquire_heap(size_t bytes);
void *acquire_mapping(size_t payload);
void release_mapping(struct block_meta *b);
struct block_meta *bootstrap_heap(void);

static inline size_t align_payload(size_t raw)
{
	return ALIGN_UP(raw);
}

static inline int use_mmap_for(size_t aligned_payload, size_t threshold)
{
	return aligned_payload + META_SIZE >= threshold;
}
