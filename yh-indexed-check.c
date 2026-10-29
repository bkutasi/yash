/* Differential test compiles the actual trie.c from the disposable source copy.
 * Small buffer implementations isolate trie algorithms from unrelated shell globals.
 */
#include "common.h"
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include "strbuf.h"
#include "util.h"
#include "lineedit/trie.c"
#include "lineedit/yh-predict.h"
void alloc_failed(void) {fputs("allocation failed\n",stderr);abort();}
xwcsbuf_T *wb_initwithmax(xwcsbuf_T *buf,size_t max)
{
    buf->contents=xmalloce(max,1,sizeof(wchar_t));buf->contents[0]=0;
    buf->length=0;buf->maxlength=max;return buf;
}
xwcsbuf_T *wb_setmax(xwcsbuf_T *buf,size_t max)
{
    buf->contents=xrealloce(buf->contents,max,1,sizeof(wchar_t));
    buf->maxlength=max;if(buf->length>max)buf->length=max;
    buf->contents[buf->length]=0;return buf;
}
xwcsbuf_T *wb_ensuremax(xwcsbuf_T *buf,size_t max)
{
    if(max>buf->maxlength)
        wb_setmax(buf,max);
    return buf;
}
xwcsbuf_T *wb_wccat(xwcsbuf_T *buf,wchar_t c)
{
    wb_ensuremax(buf,add(buf->length,1));buf->contents[buf->length++]=c;
    buf->contents[buf->length]=0;return buf;
}

static uint32_t seed = 0x5eeda11u;
static uint32_t random_word(void) {seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static size_t comparisons;
static void compare_query(const trie_T *reference, const yh_pred_T *indexed, const wchar_t *prefix)
{
    wchar_t *a=trie_probable_key(reference,prefix), *b=yh_pred_predict(indexed,prefix);
    if(wcscmp(a,b)) {fputs("FAIL: prediction mismatch (query text suppressed)\n",stderr);abort();}
    free(a);free(b);comparisons++;
}
static void compare_frozen(const trienode_T *reference,const yh_pred_T *tree,uint32_t segment,uint32_t position)
{
    const yh_pred_segment_T *part=&tree->segments[segment];
    if(reference->valuevalid)assert(memcmp(&reference->value.probability,&part->probability,sizeof(double))==0);
    if(position<part->length) {
        assert(reference->count==1);
        assert(reference->entries[0].key.as_wchar==yh_pred_label(tree,part->offset+position));
        compare_frozen(reference->entries[0].child,tree,segment,position+1);
    } else {
        uint32_t child=part->child;
        for(size_t i=0;i<reference->count;i++) {
            assert(child && tree->segments[child].length);
            assert(reference->entries[i].key.as_wchar==yh_pred_label(tree,tree->segments[child].offset));
            compare_frozen(reference->entries[i].child,tree,child,1);
            child=tree->segments[child].next;
        }
        assert(!child);
    }
}
static void compare_nodes(const trienode_T *reference,const yh_pred_T *indexed,uint32_t index)
{
    if(indexed->segments) {assert(index==0);compare_frozen(reference,indexed,0,0);return;}
    if(reference->valuevalid)assert(memcmp(&reference->value.probability,&indexed->probabilities[index],sizeof(double))==0);
    uint32_t child=indexed->nodes[index].child;
    for(size_t i=0;i<reference->count;i++) {
        assert(child && indexed->nodes[child].key==reference->entries[i].key.as_wchar);
        compare_nodes(reference->entries[i].child,indexed,child);
        child=indexed->nodes[child].next;
    }
    assert(child==0);
}
static void edge_cases(void)
{
    static const wchar_t *keys[]={L"",L"a",L"ab",L"ac",L"a b",L"a\tb",L"\u00e9",L"\u4e2d",L"\U0001f600",L"a\u3000b"};
    for(size_t reverse=0;reverse<2;reverse++) {
        trie_T *r=trie_create();yh_pred_T *p=yh_pred_create();
        compare_query(r,p,L"");compare_query(r,p,L"missing");
        for(size_t j=0;j<sizeof keys/sizeof *keys;j++) {
            size_t i=reverse?sizeof keys/sizeof *keys-j-1:j;
            r=trie_add_probability(r,keys[i],1.0);p=yh_pred_add(p,keys[i],1.0);
            compare_query(r,p,L"");compare_query(r,p,L"a");compare_query(r,p,L"a ");
        }
        compare_nodes(r,p,0);
        for(size_t j=0;j<sizeof keys/sizeof *keys;j++)compare_query(r,p,keys[j]);
        /* Exercise growth before freezing; production never mutates a frozen tree. */
        r=trie_add_probability(r,L"new after finish",.125);p=yh_pred_add(p,L"new after finish",.125);
        p=yh_pred_finish(p);
        compare_query(r,p,L"new ");compare_nodes(r,p,0);
        trie_destroy(r);yh_pred_destroy(p);
    }
    yh_pred_destroy(NULL);
    for(size_t width=1;width<=2;width++) {
        trie_T *r=trie_create();yh_pred_T *p=yh_pred_create();
        wchar_t keys[32][96];
        for(size_t i=0;i<32;i++) {
            for(size_t j=0;j<64;j++)keys[i][j]=width==1?(wchar_t)(0x80+j%32):(wchar_t)(0x4e00+j%32);
            keys[i][64]=(wchar_t)(L'a'+i);keys[i][65]=L' ';keys[i][66]=L'x';keys[i][67]=0;
            r=trie_add_probability(r,keys[i],1.0/(i+2));p=yh_pred_add(p,keys[i],1.0/(i+2));
        }
        p=yh_pred_finish(p);assert(p->segments && p->label_width==width);compare_nodes(r,p,0);
        for(size_t i=0;i<32;i++)for(size_t j=0;j<=67;j++) {
            wchar_t saved=keys[i][j];keys[i][j]=0;compare_query(r,p,keys[i]);keys[i][j]=saved;
        }
        trie_destroy(r);yh_pred_destroy(p);
    }
    static const wchar_t width_values[]={0xe9,0xff,0x100,0x4e2d,0xffff,0x10000,0x1f600,0x10ffff};
    for(size_t mode=0;mode<sizeof width_values/sizeof *width_values;mode++) {
        wchar_t key[257];wchar_t value=width_values[mode];
        for(size_t i=0;i<256;i++)key[i]=value;
        key[256]=0;
        trie_T *r=trie_add_probability(trie_create(),key,.25);
        yh_pred_T *p=yh_pred_finish(yh_pred_add(yh_pred_create(),key,.25));
        assert(p->segments && p->label_width==(value<=0xff?1:value<=0xffff?2:sizeof(wchar_t)));
        compare_nodes(r,p,0);
        for(size_t i=0;i<=256;i++) {wchar_t saved=key[i];key[i]=0;compare_query(r,p,key);key[i]=saved;}
        trie_destroy(r);yh_pred_destroy(p);
    }
    {
        trie_T *r=trie_create();yh_pred_T *p=yh_pred_finish(yh_pred_create());
        compare_query(r,p,L"");compare_query(r,p,L"missing");compare_nodes(r,p,0);
        p=yh_pred_finish(p);compare_query(r,p,L"");
        trie_destroy(r);yh_pred_destroy(p);
    }
    {
        trie_T *r=trie_create();yh_pred_T *p=yh_pred_create();
        static const wchar_t *keys[]={L"a",L"ab",L"abc",L"abc ",L"abc x",L"abc y",L"abc\tq",L"abz",L"b"};
        for(size_t i=0;i<sizeof keys/sizeof *keys;i++) {
            double w=i%3==0?1.0:1.0/(i+2);
            r=trie_add_probability(r,keys[i],w);p=yh_pred_add(p,keys[i],w);
        }
        p=yh_pred_finish(p);compare_nodes(r,p,0);
        for(size_t i=0;i<sizeof keys/sizeof *keys;i++) {
            wchar_t key[32];wcscpy(key,keys[i]);size_t n=wcslen(key);
            for(size_t j=0;j<=n;j++) {wchar_t saved=key[j];key[j]=0;compare_query(r,p,key);key[j]=saved;}
        }
        trie_destroy(r);yh_pred_destroy(p);
    }
    {
        trie_T *r=trie_create();yh_pred_T *p=yh_pred_create();
        r=trie_add_probability(r,L"x",1.0);p=yh_pred_add(p,L"x",1.0);
        p=yh_pred_finish(p);compare_nodes(r,p,0);compare_query(r,p,L"");
        trie_destroy(r);yh_pred_destroy(p);
    }
    {
        trie_T *r=trie_create();yh_pred_T *p=yh_pred_create();
        for(size_t i=0;i<1024;i++) {
            wchar_t key[2]={(wchar_t)(0x400+i),0};
            r=trie_add_probability(r,key,1.0);p=yh_pred_add(p,key,1.0);
        }
        p=yh_pred_finish(p);compare_nodes(r,p,0);compare_query(r,p,L"");
        for(size_t i=0;i<1024;i++) {
            wchar_t key[2]={(wchar_t)(0x400+i),0};compare_query(r,p,key);
        }
        trie_destroy(r);yh_pred_destroy(p);
    }
    {
        static wchar_t key[4097];
        for(size_t i=0;i<4096;i++)key[i]=(wchar_t)(L'a'+i%26);
        trie_T *r=trie_add_probability(trie_create(),key,.5);
        yh_pred_T *p=yh_pred_finish(yh_pred_add(yh_pred_create(),key,.5));
        compare_nodes(r,p,0);compare_query(r,p,L"");compare_query(r,p,key);
        key[2048]=0;compare_query(r,p,key);
        trie_destroy(r);yh_pred_destroy(p);
    }
}
int main(int argc,char **argv)
{
    const char *locale=getenv("YH_TEST_LOCALE");
    assert(setlocale(LC_ALL,locale?locale:"C.UTF-8")!=NULL);
    const char *test_seed=getenv("YH_TEST_SEED");
    if(test_seed)seed=(uint32_t)strtoul(test_seed,NULL,10);
    if(argc==1)edge_cases();
    bool memory=argc>1;
    bool indexed_only=memory&&!strcmp(argv[1],"indexed");
    bool reference_only=memory&&!strcmp(argv[1],"reference");
    assert(!memory||indexed_only||reference_only);
    trie_T *r=indexed_only?NULL:trie_create();
    yh_pred_T *p=reference_only?NULL:yh_pred_create();
    enum {SAMPLES=2000,WIDTH=96};
    static wchar_t samples[SAMPLES][WIDTH];
    static const wchar_t alphabet[]=L"abcde0123 /\t\u00e9\u4e2d\U0001f600\u3000";
    for(size_t i=0;i<SAMPLES;i++) {
        size_t length=random_word()%(WIDTH-1);
        for(size_t j=0;j<length;j++)samples[i][j]=alphabet[random_word()%(sizeof alphabet/sizeof *alphabet-1)];
        samples[i][length]=0;
        if(i%7==0&&i>0)wcscpy(samples[i],samples[i-1]);
        double weight=1.0/(i+2);
        if(r)r=trie_add_probability(r,samples[i],weight);
        if(p)p=yh_pred_add(p,samples[i],weight);
        if(!memory&&i%17==0)compare_query(r,p,L"");
    }
    if(p)p=yh_pred_finish(p);
    if(!memory) {
        compare_nodes(r,p,0);
        for(size_t i=0;i<SAMPLES;i++) {
            size_t length=wcslen(samples[i]);
            for(size_t j=0;j<=length;j++) {
                wchar_t saved=samples[i][j];samples[i][j]=0;
                compare_query(r,p,samples[i]);samples[i][j]=saved;
            }
        }
        for(size_t i=0;i<20000;i++) {
            wchar_t query[8];size_t length=random_word()%7;
            for(size_t j=0;j<length;j++)query[j]=alphabet[random_word()%(sizeof alphabet/sizeof *alphabet-1)];
            query[length]=0;compare_query(r,p,query);
        }
        printf("PASS: %zu differential suffix queries; bit-identical node probabilities; ties, prefixes, duplicates, blanks, Unicode, reallocations, empty tree and destruction\n",comparisons);
    } else {
        if(p)printf("representation=%s nodes=%zu segments=%zu labels=%zu requested_final_bytes=%zu\n",p->segments?"radix":"SoA",p->count,p->segment_count,p->label_count,p->segments?p->segment_count*sizeof(yh_pred_segment_T)+p->label_count*p->label_width:p->count*(sizeof(yh_pred_node_T)+sizeof(double)));
        printf("Synthetic corpus only; mode=%s; samples=%d; indexed_node_bytes=%zu\n",argv[1],SAMPLES,sizeof(yh_pred_node_T)+sizeof(double));
        FILE *f=fopen("/proc/self/smaps_rollup","r");assert(f);char line[256];
        while(fgets(line,sizeof line,f))if(!strncmp(line,"Rss:",4)||!strncmp(line,"Pss:",4)||!strncmp(line,"Private_Dirty:",14))fputs(line,stdout);
        fclose(f);
    }
    trie_destroy(r);yh_pred_destroy(p);
    return 0;
}
