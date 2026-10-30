#define _GNU_SOURCE 1
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#include <assert.h>
typedef struct histlink_T {struct histlink_T *prev,*next;} histlink_T;
typedef struct histentry_T {histlink_T link;int64_t time;unsigned number;uint32_t value_id;} histentry_T;
static size_t add(size_t a,size_t b){if(a>SIZE_MAX-b)abort();return a+b;}
static size_t mul(size_t a,size_t b){if(b&&a>SIZE_MAX/b)abort();return a*b;}
static void *xmalloc(size_t n){void *p=malloc(n);if(!p)abort();return p;}
static void *xreallocn(void *p,size_t n,size_t size){void *q=realloc(p,mul(n,size));if(!q)abort();return q;}
#include "yh-cold-store.h"
static uint32_t rng=0x517acafe;
static uint32_t random_value(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void codec_check(void)
{
    unsigned char source[32768],encoded[36866],decoded[32768];
    for(size_t round=0;round<1000;round++){
        size_t n=round<20?round:random_value()%sizeof source;
        for(size_t i=0;i<n;i++)source[i]=(unsigned char)(round%3?random_value()%8:random_value());
        size_t bytes=yh_cold_encode(source,n,encoded,sizeof encoded);
        assert(bytes||!n);assert(yh_cold_decode(encoded,bytes,decoded,n));assert(!memcmp(source,decoded,n));
        if(bytes){assert(!yh_cold_decode(encoded,bytes-1,decoded,n));}
    }
    const unsigned char invalid[][4]={{1,0,0,0},{1,1,0,0},{0,1,1,1}};
    for(size_t i=0;i<2;i++)assert(!yh_cold_decode(invalid[i],4,decoded,4));
    for(size_t round=0;round<20000;round++){
        size_t n=random_value()%64,out=random_value()%256;
        for(size_t i=0;i<n;i++)encoded[i]=(unsigned char)random_value();
        (void)yh_cold_decode(encoded,n,decoded,out); /* Bounded malformed-stream fuzz. */
    }
}
static void nested_check(histentry_T *entry,const char *expected)
{
    YH_HIST_SCOPE;
    assert(!strcmp(yh_hist_value(entry),expected));
}
static void store_check(void)
{
    enum {N=23337};histentry_T *entries=xmalloc(N*sizeof *entries);char **expected=xmalloc(N*sizeof *expected);
    for(size_t i=0;i<N;i++){
        char line[4096];
        if(i%11)snprintf(line,sizeof line,"git -C /home/user/projects/project-%zu status --branch --short && printf item-%zu",i%71,i);
        else{size_t n=1+random_value()%3000;for(size_t j=0;j<n;j++)line[j]=(char)(33+random_value()%90);line[n]=0;}
        size_t n=strlen(line)+1;expected[i]=xmalloc(n);memcpy(expected[i],line,n);
        entries[i]=(histentry_T){.number=(unsigned)i,.time=5000000000LL+(int64_t)i,.value_id=yh_hist_put(line)};
    }
    yh_hist_cold_finish();
    for(size_t i=0;i<N;i++){
        YH_HIST_SCOPE;
        const char *text=yh_hist_value(&entries[i]);nested_check(&entries[(i+1)%N],expected[(i+1)%N]);
        assert(!strcmp(text,expected[i]));
    }
    /* Many simultaneously borrowed blocks remain pinned until the outer scope ends. */
    {
        YH_HIST_SCOPE;const char *borrowed[100];
        for(size_t i=0;i<100;i++)borrowed[i]=yh_hist_value(&entries[i*211]);
        for(size_t i=0;i<100;i++)assert(!strcmp(borrowed[i],expected[i*211]));
        /* Delete an entry while a caller still borrows its text. */
        const char *last=yh_hist_value(&entries[N-1]);yh_hist_drop(entries[N-1].value_id);
        assert(!strcmp(last,expected[N-1]));entries[N-1].value_id=0;
    }
    /* Borrow open-buffer text, force sealing, then verify the old pointer. */
    {
        YH_HIST_SCOPE;histentry_T small={.value_id=yh_hist_put("open-buffer-stable")};
        const char *borrowed=yh_hist_value(&small);char *large=xmalloc(YH_COLD_BLOCK);memset(large,'q',YH_COLD_BLOCK-1);large[YH_COLD_BLOCK-1]=0;
        uint32_t id=yh_hist_put(large);assert(!strcmp(borrowed,"open-buffer-stable"));
        yh_hist_drop(id);yh_hist_drop(small.value_id);free(large);
    }
    /* Empty, whitespace and non-ASCII command bytes are not normalized. */
    const char *special[]={"", " ", "\303\241rv\303\255z \344\270\226\347\225\214", "echo duplicate", "echo duplicate"};
    for(size_t i=0;i<sizeof special/sizeof *special;i++){
        histentry_T entry={.value_id=yh_hist_put(special[i])};
        {YH_HIST_SCOPE;assert(!strcmp(yh_hist_value(&entry),special[i]));}
        yh_hist_drop(entry.value_id);
    }
    /* Oversized commands retain exact bytes through raw fallback. */
    char *large=xmalloc(YH_COLD_BLOCK+123);memset(large,'z',YH_COLD_BLOCK+122);large[YH_COLD_BLOCK+122]=0;
    histentry_T raw={.value_id=yh_hist_put(large)};
    {YH_HIST_SCOPE;assert(!strcmp(yh_hist_value(&raw),large));}
    size_t enc_now,dec_now,live_now,blocks_now;yh_hist_cold_stats(&enc_now,&dec_now,&live_now,&blocks_now);
    assert(dec_now<=mul((size_t)YH_COLD_CACHE,YH_COLD_BLOCK));
    yh_hist_drop(raw.value_id);free(large);
    for(size_t i=0;i<N;i++){
        if(entries[i].value_id)yh_hist_drop(entries[i].value_id);
        free(expected[i]);
    }
    free(expected);free(entries);yh_hist_cold_finish();
    for(size_t i=1;i<yh_cold_count;i++)assert(!yh_cold_blocks[i]);
    free(yh_cold_blocks);yh_cold_blocks=NULL;yh_cold_count=1;yh_cold_capacity=0;
}
static void memory_demo(void)
{
    enum {N=23337};histentry_T *entries=xmalloc(N*sizeof *entries);size_t text_bytes=0;
    for(size_t i=0;i<N;i++){
        char text[256];snprintf(text,sizeof text,"git -C /home/user/project-%zu status --short --branch item-%zu",i%31,i);
        text_bytes+=strlen(text)+1;entries[i]=(histentry_T){.value_id=yh_hist_put(text)};
    }
    yh_hist_cold_finish();size_t encoded,decoded,live,blocks;yh_hist_cold_stats(&encoded,&decoded,&live,&blocks);
    printf("SYNTHETIC ONLY: entries=%u text_bytes=%zu encoded_store_bytes=%zu decoded_cache_bytes=%zu metadata_bytes=%zu block_count=%zu\n",N,text_bytes,encoded,decoded,N*sizeof *entries,blocks);
    for(size_t i=0;i<N;i++)yh_hist_drop(entries[i].value_id);
    free(entries);free(yh_cold_blocks);yh_cold_blocks=NULL;yh_cold_count=1;yh_cold_capacity=0;
}
static int corpus_probe(const char *path)
{
    FILE *file=fopen(path,"rb");if(!file){fputs("Cannot open private corpus\n",stderr);return 2;}
    uint32_t *ids=NULL;size_t count=0,capacity=0,text_bytes=0,line_capacity=0;char *line=NULL;ssize_t n;
    while((n=getline(&line,&line_capacity,file))>=0){
        if(n&&line[n-1]=='\n')line[--n]=0;
        if(memchr(line,0,(size_t)n)){fputs("NUL in history export; rejected\n",stderr);return 2;}
        if(count==capacity){capacity=capacity?mul(capacity,2):1024;ids=xreallocn(ids,capacity,sizeof *ids);}
        ids[count++]=yh_hist_put(line);text_bytes=add(text_bytes,add((size_t)n,1));
    }
    if(ferror(file)){fputs("Corpus read failed\n",stderr);return 2;}
    yh_hist_cold_finish();size_t encoded,decoded,live,blocks;yh_hist_cold_stats(&encoded,&decoded,&live,&blocks);
    if(live!=text_bytes)abort();
    rewind(file);size_t index=0;
    while((n=getline(&line,&line_capacity,file))>=0){
        if(n&&line[n-1]=='\n')line[--n]=0;
        if(index>=count)abort();
        histentry_T entry={.value_id=ids[index++]};
        {YH_HIST_SCOPE;if(strcmp(yh_hist_value(&entry),line))abort();}
    }
    if(ferror(file)||index!=count)abort();
    fclose(file);free(line);
    printf("{\"actual_export_entries\":%zu,\"actual_text_bytes\":%zu,\"encoded_store_requested_bytes\":%zu,\"decoded_cache_bytes_at_build\":%zu,\"metadata_logical_bytes\":%zu,\"blocks\":%zu,\"roundtrip_verified\":true}\n",
        count,text_bytes,encoded,decoded,mul(count,sizeof(histentry_T)),blocks);
    for(size_t i=0;i<count;i++)yh_hist_drop(ids[i]);
    free(ids);
    for(size_t i=1;i<yh_cold_count;i++)if(yh_cold_blocks[i])abort();
    free(yh_cold_blocks);return 0;
}
int main(int argc,char **argv)
{
    if(argc==3&&!strcmp(argv[1],"--corpus"))return corpus_probe(argv[2]);
    if(argc>1)rng=(uint32_t)strtoul(argv[1],NULL,10);
    codec_check();store_check();memory_demo();
    puts("PASS: codec roundtrips/malformed streams, 23337 records, nested scopes, pinned pointers, deletion, sealing, raw fallback and complete block release");return 0;
}
