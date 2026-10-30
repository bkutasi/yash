/* Experimental history-entry pool, GPL-2.0-or-later.
 * Single-threaded shell only. No pointer moves, no history truncation.
 * Include after project util.h and standard allocation headers.
 */
#ifndef YH_HISTORY_POOL_V5
#define YH_HISTORY_POOL_V5
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#if UINTPTR_MAX != UINT64_MAX
#error "V5 pool currently validated only for 64-bit pointers"
#endif

typedef struct yh_hist_block {
    struct yh_hist_block *next;
    void *free_head;
    size_t stride, capacity, used, virgin;
    /* malloc alignment plus this union keeps records naturally aligned. */
    union {long double ld;void *ptr;long long ll;} alignment;
    unsigned char data[];
} yh_hist_block;

#define YH_HIST_LIMIT 1024u
#define YH_HIST_STEP sizeof(void *)
#define YH_HIST_CLASSES (YH_HIST_LIMIT / YH_HIST_STEP)
static yh_hist_block *yh_hist_bins[YH_HIST_CLASSES];
static unsigned char *yh_hist_bitmap(yh_hist_block *block)
{
    return block->data + block->capacity * block->stride;
}
static size_t yh_hist_block_bytes(const yh_hist_block *block)
{
    return add(add(offsetof(yh_hist_block, data), mul(block->capacity, block->stride)),
        add(block->capacity, 7) / 8);
}

static void *yh_hist_alloc(size_t size)
{
    if (!size || size > YH_HIST_LIMIT)
        return xmalloc(size ? size : 1);
    size_t stride = mul((add(size, YH_HIST_STEP - 1) / YH_HIST_STEP), YH_HIST_STEP);
    size_t bin = stride / YH_HIST_STEP - 1;
    yh_hist_block *block = yh_hist_bins[bin];
    while (block && block->used == block->capacity)
        block = block->next;
    if (!block) {
        /* Keep the total request at or below 4080 bytes, including headers,
         * to avoid crossing a page-sized allocator class for metadata. Larger
         * blocks may be worthwhile if measured allocation throughput dominates.
         */
        size_t capacity = (4080 - offsetof(yh_hist_block, data)) / stride;
        while (add(add(offsetof(yh_hist_block, data), mul(capacity, stride)),
                add(capacity, 7) / 8) > 4080)
            capacity--;
        block = xmalloc(add(add(offsetof(yh_hist_block, data), mul(capacity, stride)), add(capacity, 7) / 8));
        block->next = yh_hist_bins[bin];
        block->free_head = NULL;
        block->stride = stride;
        block->capacity = capacity;
        block->used = block->virgin = 0;
        yh_hist_bins[bin] = block;
        memset(yh_hist_bitmap(block), 0, add(capacity, 7) / 8);
    }
    void *result;
    if (block->free_head) {
        result = block->free_head;
        uintptr_t address = (uintptr_t)result, start = (uintptr_t)block->data;
        if (address < start || address - start >= block->capacity * stride ||
                (address - start) % stride)
            abort();
        memcpy(&block->free_head, result, sizeof(void *));
    } else {
        if (block->virgin >= block->capacity)
            abort();
        result = block->data + block->virgin++ * stride;
    }
    size_t index = ((unsigned char *)result - block->data) / block->stride;
    unsigned char *bitmap = yh_hist_bitmap(block);
    unsigned char bit = (unsigned char)(1u << (index % 8));
    if (bitmap[index / 8] & bit)
        abort();
    bitmap[index / 8] |= bit;
    block->used++;
    return result;
}

static void yh_hist_free(void *pointer)
{
    if (!pointer)
        return;
    uintptr_t address = (uintptr_t)pointer;
    /* No metadata is read before an arbitrary pointer. Non-pool allocations
     * retain libc ownership. Search is bounded by currently allocated blocks.
     * Freeing is O(blocks); an address index is only worthwhile if measured
     * deletion latency matters. Never dereference a guessed allocation header.
     */
    for (size_t bin = 0; bin < YH_HIST_CLASSES; bin++) {
        yh_hist_block **link = &yh_hist_bins[bin];
        while (*link) {
            yh_hist_block *block = *link;
            uintptr_t start = (uintptr_t)block->data;
            size_t bytes = mul(block->capacity, block->stride);
            if (address >= start && address - start < bytes) {
                size_t offset = address - start;
                size_t index = offset / block->stride;
                unsigned char *bitmap = yh_hist_bitmap(block);
                unsigned char bit = (unsigned char)(1u << (index % 8));
                if (offset % block->stride || index >= block->virgin ||
                        !block->used || !(bitmap[index / 8] & bit))
                    abort();
                bitmap[index / 8] &= (unsigned char)~bit;
#ifdef YH_HIST_RELEASE
                YH_HIST_RELEASE(pointer);
#endif
                memcpy(pointer, &block->free_head, sizeof(void *));
                block->free_head = pointer;
                if (--block->used == 0) {
                    *link = block->next;
                    free(block);
                }
                return;
            }
            link = &block->next;
        }
    }
    free(pointer);
}
/* Aggregate accounting only; never exposes command text. */
void yh_hist_pool_stats(size_t *allocated, size_t *live, size_t *blocks)
{
    *allocated = *live = *blocks = 0;
    for (size_t bin = 0; bin < YH_HIST_CLASSES; bin++)
        for (yh_hist_block *block = yh_hist_bins[bin]; block; block = block->next) {
            *allocated = add(*allocated, yh_hist_block_bytes(block));
            *live = add(*live, mul(block->used, block->stride));
            (*blocks)++;
        }
}
#endif
