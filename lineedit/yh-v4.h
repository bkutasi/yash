/* Yash prediction semantics: (C) 2007-2016 magicant.
 * V4 compact frozen representation: GPL-2.0-or-later.
 * Include AFTER the existing v3 yh-predict.h and project util/strbuf headers.
 */
#ifndef YH_COMPACT_PREDICTION_V4
#define YH_COMPACT_PREDICTION_V4
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <wctype.h>

typedef struct {
    yh_pred_T *build;
    double *probabilities;
    uint32_t *offsets;
    void *links, *storage;
    unsigned char *widths, *labels;
    size_t count, bytes;
    unsigned link_width;
} yh_v4_T;

static yh_v4_T *yh_v4_create(void)
{
    yh_v4_T *t = xmalloc(sizeof *t);
    *t = (yh_v4_T){.build = yh_pred_create()};
    return t;
}

static yh_v4_T *yh_v4_add(yh_v4_T *t, const wchar_t *key, double probability)
{
    assert(t->build && !t->count);
    t->build = yh_pred_add(t->build, key, probability);
    return t;
}

static unsigned yh_v4_link_width(size_t count)
{
    return count <= (size_t)UINT16_MAX + 1 ? sizeof(uint16_t) : sizeof(uint32_t);
}

static uint32_t yh_v4_link(const yh_v4_T *t, size_t index, unsigned next)
{
    size_t slot = index * 2 + next;
    return t->link_width == sizeof(uint16_t) ?
        ((const uint16_t *)t->links)[slot] : ((const uint32_t *)t->links)[slot];
}

static void yh_v4_set_link(yh_v4_T *t, size_t index, unsigned next, uint32_t value)
{
    size_t slot = index * 2 + next;
    if (t->link_width == sizeof(uint16_t)) {
        assert(value <= UINT16_MAX);
        ((uint16_t *)t->links)[slot] = (uint16_t)value;
    } else
        ((uint32_t *)t->links)[slot] = value;
}

static unsigned yh_v4_segment_width(const yh_pred_T *source, size_t segment)
{
    const yh_pred_segment_T *part = &source->segments[segment];
    unsigned width = 1;
    for (uint32_t i = 0; i < part->length; i++) {
        wchar_t key = yh_pred_label(source, (size_t)part->offset + i);
        /* Negative wchar_t values also retain full width. */
        if ((uintmax_t)key > UINT16_MAX)
            return sizeof(wchar_t);
        if ((uintmax_t)key > UINT8_MAX)
            width = sizeof(uint16_t);
    }
    return width;
}

static yh_v4_T *yh_v4_finish(yh_v4_T *t)
{
    if (t->count)
        return t;
    t->build = yh_pred_finish(t->build);
    const yh_pred_T *source = t->build;
    if (!source->segments)
        return t;
    size_t count = source->segment_count;
    if (!count || count > UINT32_MAX)
        return t;
    size_t label_bytes = 0;
    for (size_t i = 0; i < count; i++) {
        unsigned width = yh_v4_segment_width(source, i);
        label_bytes = add(label_bytes, mul(source->segments[i].length, width));
        /* ponytail: 32-bit byte offsets; retain v3 beyond 4 GiB, never truncate. */
        if (label_bytes > UINT32_MAX)
            return t;
    }
    unsigned link_width = yh_v4_link_width(count);
    size_t old_bytes = add(mul(count, sizeof(yh_pred_segment_T)),
        mul(source->label_count, source->label_width));
    size_t new_bytes = add(label_bytes, add(mul(count, sizeof(double)),
        add(mul(add(count, 1), sizeof(uint32_t)),
            add(mul(mul(count, 2), link_width), count))));
    /* Include the wrapper object; do not call a larger requested layout a win. */
    if (add(new_bytes, sizeof *t) >= old_bytes)
        return t;
    /* One allocation, laid out in decreasing alignment. No packed typed loads.
     * double array starts at malloc alignment; following 32/16-bit arrays remain
     * naturally aligned because all preceding sizes are multiples of four.
     */
    t->storage = xmalloc(new_bytes);
    unsigned char *cursor = t->storage;
    t->probabilities = (double *)cursor;
    cursor += mul(count, sizeof(double));
    t->offsets = (uint32_t *)cursor;
    cursor += mul(add(count, 1), sizeof(uint32_t));
    t->links = cursor;
    cursor += mul(mul(count, 2), link_width);
    t->widths = cursor;
    cursor += count;
    t->labels = cursor;
    t->count = count;
    t->link_width = link_width;
    t->bytes = new_bytes;
    size_t offset = 0;
    for (size_t i = 0; i < count; i++) {
        const yh_pred_segment_T *part = &source->segments[i];
        unsigned width = yh_v4_segment_width(source, i);
        t->widths[i] = (unsigned char)width;
        t->offsets[i] = (uint32_t)offset;
        /* Copy probability bits, do not re-sum or quantize. */
        memcpy(&t->probabilities[i], &part->probability, sizeof(double));
        yh_v4_set_link(t, i, 0, part->child);
        yh_v4_set_link(t, i, 1, part->next);
        for (uint32_t j = 0; j < part->length; j++) {
            wchar_t key = yh_pred_label(source, (size_t)part->offset + j);
            if (width == 1)
                t->labels[offset] = (unsigned char)key;
            else if (width == sizeof(uint16_t)) {
                uint16_t value = (uint16_t)key;
                memcpy(t->labels + offset, &value, sizeof value);
            } else
                memcpy(t->labels + offset, &key, sizeof key);
            offset += width;
        }
    }
    assert(offset == label_bytes);
    t->offsets[count] = (uint32_t)offset;
    yh_pred_destroy(t->build);
    t->build = NULL;
    return t;
}

static size_t yh_v4_length(const yh_v4_T *t, uint32_t index)
{
    return (t->offsets[index + 1] - t->offsets[index]) / t->widths[index];
}

static wchar_t yh_v4_label(const yh_v4_T *t, uint32_t index, size_t position)
{
    const unsigned char *p = t->labels + t->offsets[index] + position * t->widths[index];
    if (t->widths[index] == 1)
        return (wchar_t)*p;
    if (t->widths[index] == sizeof(uint16_t)) {
        uint16_t value;
        memcpy(&value, p, sizeof value);
        return (wchar_t)value;
    }
    wchar_t value;
    memcpy(&value, p, sizeof value);
    return value;
}

static uint32_t yh_v4_best(const yh_v4_T *t, uint32_t index)
{
    uint32_t best = 0;
    double probability = -HUGE_VAL;
    for (uint32_t child = yh_v4_link(t, index, 0); child; child = yh_v4_link(t, child, 1)) {
        if (t->probabilities[child] > probability) {
            probability = t->probabilities[child];
            best = child;
        }
    }
    return best;
}

static wchar_t *yh_v4_predict(const yh_v4_T *t, const wchar_t *prefix)
{
    if (t->build)
        return yh_pred_predict(t->build, prefix);
    uint32_t segment = 0;
    size_t position = 0, length = yh_v4_length(t, segment);
    while (*prefix) {
        if (position == length) {
            uint32_t child = yh_v4_link(t, segment, 0);
            while (child && yh_v4_label(t, child, 0) < *prefix)
                child = yh_v4_link(t, child, 1);
            if (!child || yh_v4_label(t, child, 0) != *prefix)
                return xwcsdup(L"");
            segment = child;
            position = 0;
            length = yh_v4_length(t, segment);
        }
        if (yh_v4_label(t, segment, position) != *prefix)
            return xwcsdup(L"");
        position++;
        prefix++;
    }
    if (position == length) {
        segment = yh_v4_best(t, segment);
        position = 0;
        if (!segment)
            return xwcsdup(L"");
        length = yh_v4_length(t, segment);
    }
    double threshold = t->probabilities[segment] / 2;
    xwcsbuf_T result;
    wb_init(&result);
    while (segment) {
        wchar_t key = yh_v4_label(t, segment, position);
        if (!(t->probabilities[segment] >= threshold || iswblank(key)))
            break;
        wb_wccat(&result, key);
        if (++position == length) {
            segment = yh_v4_best(t, segment);
            position = 0;
            if (segment)
                length = yh_v4_length(t, segment);
        }
    }
    return wb_towcs(&result);
}

static void yh_v4_destroy(yh_v4_T *t)
{
    if (t) {
        yh_pred_destroy(t->build);
        free(t->storage);
        free(t);
    }
}
#endif
