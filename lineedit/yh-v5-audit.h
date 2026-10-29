/* Aggregate history/predictor accounting for experimental builds only. */
#ifndef YH_V5_AUDIT_H
#define YH_V5_AUDIT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "../history.h"
#if YH_V5_POOL
extern void yh_hist_pool_stats(size_t *, size_t *, size_t *);
#endif
static void yh_v5_audit(const yh_v4_T *tree)
{
    YH_HIST_SCOPE;

    const char *path = getenv("YASH_V5_AUDIT_FILE");
    if (!path || !*path)
        return;
    size_t count = 0, text = 0;
    for (const histlink_T *link = Histlist->next; link != Histlist; link = link->next) {
        const histentry_T *entry = (const histentry_T *)link;
        count++;
        text = add(text, add(strlen(yh_hist_value(entry)), 1));
    }
    size_t pool_bytes = 0, pool_live = 0, pool_blocks = 0;
#if YH_V5_POOL
    yh_hist_pool_stats(&pool_bytes, &pool_live, &pool_blocks);
#endif
    size_t predictor = tree->bytes;
    if (tree->build) {
        const yh_pred_T *build = tree->build;
        predictor = build->segments ? add(mul(build->segment_count, sizeof(yh_pred_segment_T)),
            mul(build->label_count, build->label_width)) :
            mul(build->count, sizeof(yh_pred_node_T) + sizeof(double));
    }
    FILE *file = fopen(path, "w");
    if (!file)
        return;
    fprintf(file, "{\"history_entries\":%zu,\"text_bytes_including_nul\":%zu,"
        "\"entry_sizeof\":%zu,\"value_offset\":%zu,\"logical_entry_bytes\":%zu,"
        "\"pool_requested_bytes\":%zu,\"pool_live_slot_bytes\":%zu,\"pool_blocks\":%zu,"
        "\"predictor_requested_array_bytes\":%zu}\n", count, text,
        sizeof(histentry_T), offsetof(histentry_T, value_id),
        add(mul(count, sizeof(histentry_T)), text),
        pool_bytes, pool_live, pool_blocks, predictor);
    fclose(file);
}
#endif
