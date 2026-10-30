#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
static size_t add(size_t a,size_t b){if(a>SIZE_MAX-b)abort();return a+b;}
static size_t mul(size_t a,size_t b){if(b&&a>SIZE_MAX/b)abort();return a*b;}
static void *xmalloc(size_t n){void *p=malloc(n);if(!p)abort();return p;}
#include "yh-history-pool.h"
static uint32_t rng=0x51a5cafe;
static uint32_t random_value(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
int main(int argc,char **argv)
{
    if(argc>1&&!strcmp(argv[1],"--double-free")) {
        void *a=yh_hist_alloc(32),*b=yh_hist_alloc(32);
        (void)b;yh_hist_free(a);yh_hist_free(a);return 1;
    }
    if(argc>1&&!strcmp(argv[1],"--interior-free")) {
        unsigned char *a=yh_hist_alloc(32);yh_hist_free(a+1);return 1;
    }
    if(argc>1)rng=(uint32_t)strtoul(argv[1],NULL,10);
    enum {N=10000};void *p[N]={0};size_t lengths[N]={0};unsigned char values[N]={0};
    for(size_t round=0;round<100000;round++) {
        size_t i=random_value()%N;
        if(p[i]) {
            for(size_t j=0;j<lengths[i];j++)assert(((unsigned char *)p[i])[j]==values[i]);
            yh_hist_free(p[i]);p[i]=NULL;
        } else {
            size_t n=1+random_value()%2048;lengths[i]=n;values[i]=(unsigned char)random_value();
            p[i]=yh_hist_alloc(n);assert((uintptr_t)p[i]%sizeof(void *)==0);memset(p[i],values[i],n);
        }
    }
    for(size_t i=0;i<N;i++)if(p[i]) {
        for(size_t j=0;j<lengths[i];j++)assert(((unsigned char *)p[i])[j]==values[i]);
        yh_hist_free(p[i]);
    }
    for(size_t i=0;i<YH_HIST_CLASSES;i++)assert(!yh_hist_bins[i]);
    yh_hist_free(NULL);yh_hist_free(xmalloc(123));
    puts("PASS: 100000 pool churn operations, byte preservation, alignment, large-allocation fallback and complete block release");
    return 0;
}
