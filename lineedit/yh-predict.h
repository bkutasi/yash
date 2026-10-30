/* Prediction semantics derived from Yash trie.c, (C) 2007-2016 magicant.
 * Indexed implementation and integration: GPL-2.0-or-later.
 */
#ifndef YH_INDEXED_PREDICTION_V1
#define YH_INDEXED_PREDICTION_V1
#define YH_RADIX_PREDICTION_V3 1
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#include <math.h>
#include <string.h>

/* YH_RADIX_PREDICTION_V3: bit-exact probability-preserving path compression.
 * YH_SOA_PREDICTION_V2: separate aligned doubles remove node tail padding.
 * No packed structs, no precision reduction.
 * Prediction only: node 0 is the root and also the null child/sibling index.
 * Children stay sorted by wchar_t, preserving the original first-on-tie rule.
 * ponytail: linear sibling search; binary-search edges if wide fanout matters.
 * The uint32_t index ceiling is checked before allocating, never truncated.
 */
typedef struct {
    uint32_t child, next;
    wchar_t key;
} yh_pred_node_T;
typedef struct {
    double probability;
    uint32_t offset, length, child, next;
} yh_pred_segment_T;
typedef struct {
    yh_pred_segment_T *segments;
    unsigned char *labels;
    size_t label_width;
    size_t segment_count, label_count;
    yh_pred_node_T *nodes;
    double *probabilities;
    size_t count, capacity;
} yh_pred_T;

#ifndef YH_PRED_ALLOC
#define YH_PRED_ALLOC(n) xmalloc(n)
#define YH_PRED_RESIZE(p, n) xreallocn((p), (n), sizeof(yh_pred_node_T))
#define YH_PRED_FAILED() alloc_failed()
#endif

static yh_pred_T *yh_pred_create(void)
{
    yh_pred_T *tree = YH_PRED_ALLOC(sizeof *tree);
    tree->segments = NULL;
    tree->labels = NULL;
    tree->label_width = sizeof(wchar_t);
    tree->segment_count = tree->label_count = 0;
    tree->capacity = 64;
    tree->nodes = YH_PRED_RESIZE(NULL, tree->capacity);
    tree->probabilities = xreallocn(NULL, tree->capacity, sizeof(double));
    tree->probabilities[0] = 0;
    tree->nodes[0] = (yh_pred_node_T) {0};
    tree->count = 1;
    return tree;
}

static uint32_t yh_pred_new(yh_pred_T *tree, wchar_t key)
{
    if (tree->count >= UINT32_MAX)
        YH_PRED_FAILED();
    if (tree->count == tree->capacity) {
        size_t capacity = tree->capacity > UINT32_MAX / 2 ?
            UINT32_MAX : tree->capacity * 2;
        tree->nodes = YH_PRED_RESIZE(tree->nodes, capacity);
        tree->probabilities = xreallocn(tree->probabilities, capacity, sizeof(double));
        tree->capacity = capacity;
    }
    uint32_t index = (uint32_t) tree->count++;
    tree->probabilities[index] = 0;
    tree->nodes[index] = (yh_pred_node_T) {.key = key};
    return index;
}

static yh_pred_T *yh_pred_add(yh_pred_T *tree, const wchar_t *key, double p)
{
    assert(tree->segments == NULL);
    uint32_t index = 0;
    for (;;) {
        tree->probabilities[index] += p;
        if (*key == L'\0')
            return tree;
        uint32_t previous = 0, child = tree->nodes[index].child;
        while (child && tree->nodes[child].key < *key) {
            previous = child;
            child = tree->nodes[child].next;
        }
        if (!child || tree->nodes[child].key != *key) {
            /* Allocation can move nodes: keep indices, never interior pointers. */
            uint32_t fresh = yh_pred_new(tree, *key);
            tree->nodes[fresh].next = child;
            if (previous)
                tree->nodes[previous].next = fresh;
            else
                tree->nodes[index].child = fresh;
            child = fresh;
        }
        index = child;
        key++;
    }
}

/* Labels use 1, 2, or sizeof(wchar_t) bytes per character for the whole tree.
 * This stores code-point values, not UTF-8 bytes. Decode before comparisons and
 * iswblank; no Unicode or locale semantics change. memcpy avoids unaligned reads.
 * ponytail: whole-tree width; per-segment widths if mixed-script labels dominate.
 */
static wchar_t yh_pred_label(const yh_pred_T *tree, size_t index)
{
    const unsigned char *p = tree->labels + index * tree->label_width;
    if (tree->label_width == 1)
        return (wchar_t)*p;
    if (tree->label_width == 2) {
        uint16_t key;
        memcpy(&key, p, sizeof key);
        return (wchar_t)key;
    }
    wchar_t key;
    memcpy(&key, p, sizeof key);
    return key;
}

static void yh_pred_store_label(unsigned char *labels, size_t width, size_t index, wchar_t key)
{
    unsigned char *p = labels + index * width;
    if (width == 1)
        *p = (unsigned char)key;
    else if (width == 2) {
        uint16_t value = (uint16_t)key;
        memcpy(p, &value, sizeof value);
    } else
        memcpy(p, &key, sizeof key);
}

/* A chain can share one probability only when the original doubles have
 * identical bits. No recomputation, rounding or terminal-prefix approximation.
 * ponytail: build with SoA then freeze; temporary old+new storage peak; retain SoA if final storage would grow.
 */
static int yh_pred_continues(const yh_pred_T *tree, uint32_t index)
{
    uint32_t child = tree->nodes[index].child;
    return child && !tree->nodes[child].next &&
        memcmp(&tree->probabilities[index], &tree->probabilities[child], sizeof(double)) == 0;
}

static yh_pred_T *yh_pred_finish(yh_pred_T *tree)
{
    if (tree->segments)
        return tree;
    size_t count = 1;
    for (size_t i = 0; i < tree->count; i++) {
        if (i && yh_pred_continues(tree, (uint32_t)i))
            continue;
        for (uint32_t child = tree->nodes[i].child; child; child = tree->nodes[child].next)
            count = add(count, 1);
    }
    size_t width = 1;
    for (size_t i = 1; i < tree->count; i++) {
        if ((uintmax_t)tree->nodes[i].key > UINT16_MAX) {
            width = sizeof(wchar_t);
            break;
        }
        if ((uintmax_t)tree->nodes[i].key > UINT8_MAX)
            width = 2;
    }
    size_t packed = add(mul(count, sizeof(yh_pred_segment_T)),
            mul(tree->count - 1, width));
    size_t plain = mul(tree->count, sizeof(yh_pred_node_T) + sizeof(double));
    if (packed >= plain) {
        tree->nodes = YH_PRED_RESIZE(tree->nodes, tree->count);
        tree->probabilities = xreallocn(tree->probabilities, tree->count, sizeof(double));
        tree->capacity = tree->count;
        return tree;
    }
    yh_pred_segment_T *segments = xreallocn(NULL, count, sizeof *segments);
    unsigned char *labels = xreallocn(NULL, tree->count - 1, width);
    uint32_t *starts = xreallocn(NULL, count, sizeof *starts);
    starts[0] = 0;
    size_t used = 1, offset = 0;
    for (size_t i = 0; i < used; i++) {
        uint32_t node = starts[i];
        uint32_t sibling = i ? segments[i].next : 0;
        segments[i] = (yh_pred_segment_T) {
            .probability = tree->probabilities[node],
            .offset = (uint32_t)offset, .next = sibling
        };
        if (i) {
            for (;;) {
                yh_pred_store_label(labels, width, offset++, tree->nodes[node].key);
                segments[i].length++;
                if (!yh_pred_continues(tree, node))
                    break;
                node = tree->nodes[node].child;
            }
        }
        uint32_t previous = 0;
        for (uint32_t child = tree->nodes[node].child; child; child = tree->nodes[child].next) {
            assert(used < count && used < UINT32_MAX);
            uint32_t fresh = (uint32_t)used++;
            starts[fresh] = child;
            segments[fresh] = (yh_pred_segment_T){0};
            if (previous)
                segments[previous].next = fresh;
            else
                segments[i].child = fresh;
            previous = fresh;
        }
    }
    assert(used == count && offset == tree->count - 1);
    free(starts);
    free(tree->nodes);
    free(tree->probabilities);
    tree->nodes = NULL;
    tree->probabilities = NULL;
    tree->segments = segments;
    tree->labels = labels;
    tree->label_width = width;
    tree->segment_count = count;
    tree->label_count = offset;
    tree->capacity = 0;
    return tree;
}

static uint32_t yh_pred_best(const yh_pred_T *tree, uint32_t index)
{
    uint32_t best = 0;
    double probability = -HUGE_VAL;
    for (uint32_t child = tree->nodes[index].child; child;
            child = tree->nodes[child].next) {
        if (tree->probabilities[child] > probability) {
            probability = tree->probabilities[child];
            best = child;
        }
    }
    return best;
}

static uint32_t yh_pred_segment_best(const yh_pred_T *tree, uint32_t index)
{
    uint32_t best = 0;
    double probability = -HUGE_VAL;
    for (uint32_t child = tree->segments[index].child; child; child = tree->segments[child].next) {
        if (tree->segments[child].probability > probability) {
            probability = tree->segments[child].probability;
            best = child;
        }
    }
    return best;
}

static wchar_t *yh_pred_radix_predict(const yh_pred_T *tree, const wchar_t *prefix)
{
    uint32_t segment = 0, position = 0;
    while (*prefix) {
        const yh_pred_segment_T *part = &tree->segments[segment];
        if (position == part->length) {
            uint32_t child = part->child;
            while (child && yh_pred_label(tree, tree->segments[child].offset) < *prefix)
                child = tree->segments[child].next;
            if (!child || yh_pred_label(tree, tree->segments[child].offset) != *prefix)
                return xwcsdup(L"");
            segment = child;
            position = 0;
            part = &tree->segments[segment];
        }
        if (yh_pred_label(tree, part->offset + position) != *prefix)
            return xwcsdup(L"");
        position++;
        prefix++;
    }
    if (position == tree->segments[segment].length) {
        segment = yh_pred_segment_best(tree, segment);
        position = 0;
        if (!segment)
            return xwcsdup(L"");
    }
    double threshold = tree->segments[segment].probability / 2;
    xwcsbuf_T result;
    wb_init(&result);
    while (segment) {
        const yh_pred_segment_T *part = &tree->segments[segment];
        wchar_t key = yh_pred_label(tree, part->offset + position);
        if (!(part->probability >= threshold || iswblank(key)))
            break;
        wb_wccat(&result, key);
        if (++position == part->length) {
            segment = yh_pred_segment_best(tree, segment);
            position = 0;
        }
    }
    return wb_towcs(&result);
}

static wchar_t *yh_pred_predict(const yh_pred_T *tree, const wchar_t *prefix)
{
    if (tree->segments)
        return yh_pred_radix_predict(tree, prefix);
    uint32_t index = 0;
    while (*prefix) {
        uint32_t child = tree->nodes[index].child;
        while (child && tree->nodes[child].key < *prefix)
            child = tree->nodes[child].next;
        if (!child || tree->nodes[child].key != *prefix)
            return xwcsdup(L"");
        index = child;
        prefix++;
    }
    uint32_t child = yh_pred_best(tree, index);
    if (!child)
        return xwcsdup(L"");
    double threshold = tree->probabilities[child] / 2;
    xwcsbuf_T result;
    wb_init(&result);
    while (child && (tree->probabilities[child] >= threshold ||
                iswblank(tree->nodes[child].key))) {
        wb_wccat(&result, tree->nodes[child].key);
        child = yh_pred_best(tree, child);
    }
    return wb_towcs(&result);
}

static void yh_pred_destroy(yh_pred_T *tree)
{
    if (tree) {
        free(tree->segments);
        free(tree->labels);
        free(tree->nodes);
        free(tree->probabilities);
        free(tree);
    }
}
#endif
