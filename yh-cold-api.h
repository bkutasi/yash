/* Experimental private-memory history store. GPL-2.0-or-later. */
#ifndef YH_COLD_API_H
#define YH_COLD_API_H
#include <stddef.h>
#include <stdint.h>
typedef struct yh_hist_scope_T {
    void *pinned;
    uint64_t id;
} yh_hist_scope_T;
extern void yh_hist_scope_enter(yh_hist_scope_T *scope);
extern void yh_hist_scope_leave(yh_hist_scope_T *scope);
extern const char *yh_hist_value_scoped(yh_hist_scope_T *scope,const histentry_T *entry);
#define yh_hist_value(entry) yh_hist_value_scoped(&yh_cold_scope,(entry))
extern void yh_hist_cold_stats(size_t *encoded,size_t *decoded,size_t *live,size_t *blocks);
#define YH_HIST_SCOPE \
    yh_hist_scope_T yh_cold_scope __attribute__((cleanup(yh_hist_scope_leave))); \
    yh_hist_scope_enter(&yh_cold_scope)
#endif
