/* Compile with actual project headers, v3 header and v4 header. */
#include "common.h"
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <time.h>
#include "strbuf.h"
#include "util.h"
#include "lineedit/yh-predict.h"
#include "lineedit/yh-v4.h"
void alloc_failed(void) {fputs("allocation failed\n",stderr);abort();}
xwcsbuf_T *wb_initwithmax(xwcsbuf_T *b,size_t max) {
    b->contents=xmalloce(max,1,sizeof(wchar_t));b->contents[0]=0;b->length=0;b->maxlength=max;return b;
}
xwcsbuf_T *wb_setmax(xwcsbuf_T *b,size_t max) {
    b->contents=xrealloce(b->contents,max,1,sizeof(wchar_t));b->maxlength=max;
    if(b->length>max)b->length=max;
    b->contents[b->length]=0;return b;
}
xwcsbuf_T *wb_ensuremax(xwcsbuf_T *b,size_t max) {
    if(max>b->maxlength)wb_setmax(b,max);
    return b;
}
xwcsbuf_T *wb_wccat(xwcsbuf_T *b,wchar_t c) {
    wb_ensuremax(b,add(b->length,1));b->contents[b->length++]=c;b->contents[b->length]=0;return b;
}

/* Synthetic differential tests only. No user history is read here. */
static uint32_t test_rng=0x51a7cafeu;
static size_t test_queries;
static uint32_t test_random(void) {test_rng^=test_rng<<13;test_rng^=test_rng>>17;test_rng^=test_rng<<5;return test_rng;}
static void test_query(const yh_pred_T *a,const yh_v4_T *b,const wchar_t *prefix)
{
    wchar_t *x=yh_pred_predict(a,prefix),*y=yh_v4_predict(b,prefix);
    if(wcscmp(x,y)){fputs("FAIL: suffix mismatch; text suppressed\n",stderr);abort();}
    free(x);free(y);test_queries++;
}
static void test_layout(const yh_pred_T *a,const yh_v4_T *b)
{
    if(!b->count)return;
    assert(a->segments&&a->segment_count==b->count);
    for(size_t i=0;i<b->count;i++) {
        const yh_pred_segment_T *s=&a->segments[i];
        assert(!memcmp(&s->probability,&b->probabilities[i],sizeof(double)));
        assert(s->child==yh_v4_link(b,i,0)&&s->next==yh_v4_link(b,i,1));
        assert(s->length==yh_v4_length(b,(uint32_t)i));
        for(uint32_t j=0;j<s->length;j++)assert(yh_pred_label(a,(size_t)s->offset+j)==yh_v4_label(b,(uint32_t)i,j));
    }
}
static void test_edges(void)
{
    yh_pred_T *a=yh_pred_finish(yh_pred_create());yh_v4_T *b=yh_v4_finish(yh_v4_create());
    test_query(a,b,L"");test_query(a,b,L"missing");test_layout(a,b);
    yh_pred_destroy(a);yh_v4_destroy(b);yh_v4_destroy(NULL);
    static const wchar_t *keys[]={L"",L"a",L"ab",L"abc",L"abc ",L"abc\tq",L"abc x",L"abc y",L"abz",L"b",L"\u00ff",L"\u0100",L"\uffff",L"\U00010000",L"\U0010ffff"};
    for(unsigned reverse=0;reverse<2;reverse++) {
        a=yh_pred_create();b=yh_v4_create();
        for(size_t j=0;j<sizeof keys/sizeof *keys;j++) {
            size_t i=reverse?sizeof keys/sizeof *keys-1-j:j;
            a=yh_pred_add(a,keys[i],1.0);b=yh_v4_add(b,keys[i],1.0);
            test_query(a,b,L"");
        }
        a=yh_pred_finish(a);b=yh_v4_finish(b);b=yh_v4_finish(b);test_layout(a,b);
        for(size_t i=0;i<sizeof keys/sizeof *keys;i++) {
            wchar_t key[64];wcscpy(key,keys[i]);size_t n=wcslen(key);
            for(size_t j=0;j<=n;j++){wchar_t save=key[j];key[j]=0;test_query(a,b,key);key[j]=save;}
        }
        yh_pred_destroy(a);yh_v4_destroy(b);
    }
    a=yh_pred_create();b=yh_v4_create();
    static const wchar_t wide_values[]={L'a',0x80,0xff,0x100,0xffff,0x10000,0x10ffff,0x4e2d,0x1f600};
    for(size_t i=0;i<sizeof wide_values/sizeof *wide_values;i++) {
        wchar_t key[4097];for(size_t j=0;j<4096;j++)key[j]=wide_values[i];key[4096]=0;
        a=yh_pred_add(a,key,.25);b=yh_v4_add(b,key,.25);
    }
    a=yh_pred_finish(a);b=yh_v4_finish(b);test_layout(a,b);test_query(a,b,L"");
    yh_pred_destroy(a);yh_v4_destroy(b);
}
static yh_pred_T *test_wide_source(size_t count)
{
    yh_pred_T *a=xmalloc(sizeof *a);*a=(yh_pred_T){.segment_count=count,.label_count=(count-1)*32,.label_width=sizeof(wchar_t)};
    a->segments=xreallocn(NULL,count,sizeof *a->segments);a->labels=xreallocn(NULL,a->label_count,a->label_width);
    a->segments[0]=(yh_pred_segment_T){.probability=(double)count,.child=1};
    for(size_t i=1;i<count;i++) {
        a->segments[i]=(yh_pred_segment_T){.probability=1.0,.offset=(uint32_t)((i-1)*32),.length=32,.next=i+1<count?(uint32_t)(i+1):0};
        for(size_t j=0;j<32;j++)yh_pred_store_label(a->labels,a->label_width,(i-1)*32+j,j?(wchar_t)L'x':(wchar_t)(0x100+i));
    }
    return a;
}
static void test_boundaries(void)
{
    assert(yh_v4_link_width(65536)==2&&yh_v4_link_width(65537)==4);
    for(size_t count=65536;count<=65537;count++) {
        yh_pred_T *a=test_wide_source(count);yh_v4_T *b=yh_v4_create();yh_pred_destroy(b->build);b->build=test_wide_source(count);b=yh_v4_finish(b);
        assert(b->count==count&&b->link_width==(count==65536?2:4));test_layout(a,b);
        test_query(a,b,L"");
        wchar_t query[2]={(wchar_t)(0x100+count-1),0};test_query(a,b,query);
        yh_pred_destroy(a);yh_v4_destroy(b);
    }
}
int main(int argc,char **argv)
{
    const char *locale=getenv("YH_TEST_LOCALE");assert(setlocale(LC_ALL,locale?locale:"C.UTF-8"));
    const char *seed=getenv("YH_TEST_SEED");if(seed)test_rng=(uint32_t)strtoul(seed,NULL,10);
    if(argc>1&&!strcmp(argv[1],"--boundary")){test_boundaries();printf("PASS: 65536/65537 segment link boundaries, labels and probability bits; queries=%zu\n",test_queries);return 0;}
    bool memory=argc>1;
    bool v4=memory&&!strcmp(argv[1],"v4");assert(!memory||v4||!strcmp(argv[1],"v3"));
    if(!memory)test_edges();
    yh_pred_T *a=v4?NULL:yh_pred_create();yh_v4_T *b=memory&&!v4?NULL:yh_v4_create();
    enum {N=2000,WIDTH=128};static wchar_t samples[N][WIDTH];
    static const wchar_t alphabet[]=L"abcde0123 /\t\u00e9\u4e2d\U0001f600\u3000";
    for(size_t i=0;i<N;i++) {
        size_t n=20+test_random()%(WIDTH-21);
        for(size_t j=0;j<n;j++) {
            /* Most commands ASCII, a minority CJK/emoji: exercise per-segment widths. */
            size_t alphabet_count=i%20?11:sizeof alphabet/sizeof *alphabet-1;
            samples[i][j]=alphabet[test_random()%alphabet_count];
        }
        samples[i][n]=0;if(i&&i%7==0)wcscpy(samples[i],samples[i-1]);
        double p=1.0/(i+2);
        if(a)a=yh_pred_add(a,samples[i],p);
        if(b)b=yh_v4_add(b,samples[i],p);
    }
    if(a)a=yh_pred_finish(a);
    if(b)b=yh_v4_finish(b);
    if(!memory) {
        test_layout(a,b);
        for(size_t i=0;i<N;i++)for(size_t j=0,n=wcslen(samples[i]);j<=n;j++) {
            wchar_t save=samples[i][j];samples[i][j]=0;test_query(a,b,samples[i]);samples[i][j]=save;
        }
        for(size_t i=0;i<20000;i++) {
            wchar_t key[9];size_t n=test_random()%8;
            for(size_t j=0;j<n;j++)key[j]=alphabet[test_random()%(sizeof alphabet/sizeof *alphabet-1)];
            key[n]=0;test_query(a,b,key);
        }
        printf("PASS: %zu v3/v4 differential queries; probability bits, links, lengths and decoded labels identical\n",test_queries);
    } else {
        size_t bytes=a?(a->segments?a->segment_count*sizeof(yh_pred_segment_T)+a->label_count*a->label_width:a->count*(sizeof(yh_pred_node_T)+sizeof(double))):b->bytes;
        printf("SYNTHETIC ONLY: version=%s requested_arrays_bytes=%zu segments=%zu link_bytes=%u\n",v4?"v4":"v3",bytes,a?a->segment_count:b->count,b?b->link_width:4);
        FILE *f=fopen("/proc/self/smaps_rollup","r");assert(f);char line[256];
        while(fgets(line,sizeof line,f))if(!strncmp(line,"Rss:",4)||!strncmp(line,"Pss:",4)||!strncmp(line,"Private_Dirty:",14))fputs(line,stdout);
        fclose(f);
    }
    yh_pred_destroy(a);yh_v4_destroy(b);return 0;
}
