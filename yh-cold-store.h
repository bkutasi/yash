/* Experimental compressed history text. GPL-2.0-or-later.
 * Include ONCE in history.c, after util.h and history.h.
 * Native history file format and command bytes are not changed.
 */
#ifndef YH_COLD_STORE_IMPL
#define YH_COLD_STORE_IMPL
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <sys/mman.h>
#include <stdio.h>
#include "yh-cold-api.h"
#ifndef YH_COLD_SHIFT
#define YH_COLD_SHIFT 14
#endif
#ifndef YH_COLD_CACHE
#define YH_COLD_CACHE 2
#endif
#if YH_COLD_SHIFT < 12 || YH_COLD_SHIFT > 15
#error "Block shift must be 12..15"
#endif
#if YH_COLD_CACHE < 0 || YH_COLD_CACHE > 16
#error "Cache blocks must be 0..16"
#endif
#define YH_COLD_BLOCK ((size_t)1 << YH_COLD_SHIFT)
#define YH_COLD_RAW UINT32_C(0x80000000)
#define YH_COLD_MASK ((uint32_t)(YH_COLD_BLOCK-1))

typedef struct yh_cold_block {
    unsigned char *data, *decoded;
    size_t bytes, length, live, refs, capacity;
    uint32_t index, checksum;
    unsigned sealed, compressed, pinned, decoded_mapped;
    uint64_t stamp;
    uint64_t last_scope;
} yh_cold_block;
static yh_cold_block **yh_cold_blocks;
static size_t yh_cold_count=1, yh_cold_capacity;
static yh_cold_block *yh_cold_open;
typedef struct yh_cold_pin {yh_cold_block *block;struct yh_cold_pin *next;} yh_cold_pin;
static uint64_t yh_cold_scope_clock;
static uint64_t yh_cold_clock;

/* Byte-oriented LZSS: control bit 0 = literal; bit 1 = distance16,length8+3.
 * Blocks are independent. This is NOT the LZ4 format or an LZ4 implementation.
 * No dictionary/library/runtime dependency and no on-disk compressed format.
 */
#ifndef YH_COLD_ZLIB
#define YH_COLD_ZLIB 0
#endif
#ifndef YH_COLD_LEVEL
#define YH_COLD_LEVEL 6
#endif
#if YH_COLD_ZLIB
#include <zlib.h>
static size_t yh_cold_encode(const unsigned char *src,size_t n,unsigned char *dst,size_t cap)
{
    if(n>ULONG_MAX||cap>ULONG_MAX)return 0;
    uLongf bytes=(uLongf)cap;
    int status=compress2(dst,&bytes,src,(uLong)n,YH_COLD_LEVEL);
    if(status==Z_MEM_ERROR)abort();
    return status==Z_OK?(size_t)bytes:0;
}
static int yh_cold_decode(const unsigned char *src,size_t n,unsigned char *dst,size_t length)
{
    if(n>ULONG_MAX||length>ULONG_MAX)return 0;
    uLongf bytes=(uLongf)length;uLong consumed=(uLong)n;
    int status=uncompress2(dst,&bytes,src,&consumed);
    return status==Z_OK&&(size_t)bytes==length&&(size_t)consumed==n;
}
#else
static uint32_t yh_cold_hash(const unsigned char *p)
{
    uint32_t v=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
    return (v*UINT32_C(2654435761))>>20;
}
static size_t yh_cold_encode(const unsigned char *src,size_t n,unsigned char *dst,size_t cap)
{
    int32_t table[4096];
    for(size_t i=0;i<4096;i++)table[i]=-1;
    size_t in=0,out=0;
    while(in<n){
        if(out==cap)return 0;
        size_t control=out++;dst[control]=0;
        for(unsigned bit=0;bit<8&&in<n;bit++){
            size_t match=0,distance=0;
            if(n-in>=4){
                uint32_t hash=yh_cold_hash(src+in);int32_t previous=table[hash];table[hash]=(int32_t)in;
                if(previous>=0&&in>(size_t)previous&&in-(size_t)previous<=UINT16_MAX){
                    size_t limit=n-in<258?n-in:258;
                    while(match<limit&&src[(size_t)previous+match]==src[in+match])match++;
                    if(match>=4)distance=in-(size_t)previous;
                }
            }
            if(distance){
                if(cap-out<3)return 0;
                dst[control]|=(unsigned char)(1u<<bit);
                dst[out++]=(unsigned char)distance;dst[out++]=(unsigned char)(distance>>8);
                dst[out++]=(unsigned char)(match-3);
                /* Fill skipped hashes to improve short-command matches. */
                size_t end=in+match;
                for(size_t at=in+1;at<end&&n-at>=4;at++)table[yh_cold_hash(src+at)]=(int32_t)at;
                in=end;
            }else{
                if(out==cap)return 0;
                dst[out++]=src[in++];
            }
        }
    }
    return out;
}
static int yh_cold_decode(const unsigned char *src,size_t n,unsigned char *dst,size_t length)
{
    size_t in=0,out=0;
    while(in<n&&out<length){
        unsigned control=src[in++];
        for(unsigned bit=0;bit<8&&out<length;bit++){
            if(control&(1u<<bit)){
                if(n-in<3)return 0;
                size_t distance=(size_t)src[in]|(size_t)src[in+1]<<8;
                size_t count=(size_t)src[in+2]+3;in+=3;
                if(!distance||distance>out||count>length-out)return 0;
                for(size_t j=0;j<count;j++){dst[out]=dst[out-distance];out++;}
            }else{
                if(in==n)return 0;
                dst[out++]=src[in++];
            }
        }
    }
    return in==n&&out==length;
}
#endif
static void yh_cold_free_decoded(yh_cold_block *block)
{
    if(!block->decoded)return;
    if(block->decoded_mapped){if(munmap(block->decoded,block->length))abort();}
    else free(block->decoded);
    block->decoded=NULL;block->decoded_mapped=0;
}
static void yh_cold_release_block(yh_cold_block *block)
{
    if(block->refs||block->pinned)abort();
    if(yh_cold_open==block)yh_cold_open=NULL;
    yh_cold_blocks[block->index]=NULL;
    free(block->data);yh_cold_free_decoded(block);free(block);
}
static void yh_cold_trim(void)
{
    for(;;){
        size_t cached=0,cached_bytes=0;yh_cold_block *oldest=NULL;
        for(size_t i=1;i<yh_cold_count;i++){
            yh_cold_block *block=yh_cold_blocks[i];
            if(block&&block->decoded&&!block->pinned){
                cached++;cached_bytes=add(cached_bytes,block->length);
                if(!oldest||block->stamp<oldest->stamp)oldest=block;
            }
        }
        if(cached<=(size_t)YH_COLD_CACHE&&cached_bytes<=mul((size_t)YH_COLD_CACHE,YH_COLD_BLOCK))return;
        yh_cold_free_decoded(oldest);
    }
}
static yh_cold_block *yh_cold_new(size_t capacity)
{
    size_t index=1;
    while(index<yh_cold_count&&yh_cold_blocks[index])index++;
    if(index>=YH_COLD_RAW)abort(); /* More than 2^31 simultaneously live blocks. */
    if(index==yh_cold_count){
        if(yh_cold_count>=yh_cold_capacity){
            size_t next=yh_cold_capacity?mul(yh_cold_capacity,2):64;
            if(next>YH_COLD_RAW)next=YH_COLD_RAW;
            yh_cold_blocks=xreallocn(yh_cold_blocks,next,sizeof *yh_cold_blocks);
            memset(yh_cold_blocks+yh_cold_capacity,0,(next-yh_cold_capacity)*sizeof *yh_cold_blocks);
            yh_cold_capacity=next;
        }
        yh_cold_count++;
    }
    yh_cold_block *block=xmalloc(sizeof *block);
    *block=(yh_cold_block){.data=xmalloc(capacity),.capacity=capacity,.index=(uint32_t)index};
    yh_cold_blocks[index]=block;return block;
}
static uint32_t yh_cold_checksum(const unsigned char *text,size_t length)
{
    uint32_t sum=UINT32_C(2166136261);
    for(size_t i=0;i<length;i++)sum=(sum^text[i])*UINT32_C(16777619);
    return sum;
}
static void yh_cold_seal(yh_cold_block *block)
{
    if(block->sealed)return;
    if(!block->length){if(!block->refs)yh_cold_release_block(block);return;}
    block->checksum=yh_cold_checksum(block->data,block->length);
    size_t cap=add(add(block->length,block->length/8),2);
    unsigned char *encoded=xmalloc(cap);
    size_t bytes=yh_cold_encode(block->data,block->length,encoded,cap);
    if(bytes&&bytes<block->length){
        block->decoded=block->data;
        block->data=xreallocn(encoded,bytes,1);block->bytes=bytes;block->compressed=1;
    }else{
        free(encoded);
        /* A pinned pointer into the open buffer must not move during sealing. */
        if(!block->pinned)block->data=xreallocn(block->data,block->length,1);
        block->bytes=block->length;
    }
    block->sealed=1;
    if(yh_cold_open==block)yh_cold_open=NULL;
    yh_cold_trim();
}
static uint32_t yh_hist_put(const char *text)
{
    size_t length=add(strlen(text),1);
    if(length>YH_COLD_BLOCK){
        yh_cold_block *raw=yh_cold_new(length);
        memcpy(raw->data,text,length);raw->length=raw->live=length;raw->refs=1;
        if(length<=(size_t)INT32_MAX)yh_cold_seal(raw);
        else{raw->bytes=length;raw->sealed=1;}
        return YH_COLD_RAW|raw->index;
    }
    if(yh_cold_open&&length>YH_COLD_BLOCK-yh_cold_open->length)yh_cold_seal(yh_cold_open);
    if(!yh_cold_open)yh_cold_open=yh_cold_new(YH_COLD_BLOCK);
    yh_cold_block *block=yh_cold_open;
    /* Encoded IDs have 31 bits; beyond their range, use lossless raw fallback.
     * This does not truncate history or silently wrap a block index.
     */
    if(block->index>=(YH_COLD_RAW>>YH_COLD_SHIFT)){
        yh_cold_seal(block);
        yh_cold_block *raw=yh_cold_new(length);
        memcpy(raw->data,text,length);raw->length=raw->live=length;raw->refs=1;
        if(length<=(size_t)INT32_MAX)yh_cold_seal(raw);
        else{raw->bytes=length;raw->sealed=1;}
        return YH_COLD_RAW|raw->index;
    }
    size_t offset=block->length;
    memcpy(block->data+offset,text,length);block->length+=length;block->live+=length;block->refs++;
    return (block->index<<YH_COLD_SHIFT)|(uint32_t)offset;
}
static yh_cold_block *yh_cold_resolve(uint32_t id,size_t *offset)
{
    uint32_t index;
    if(id&YH_COLD_RAW){index=id&~YH_COLD_RAW;*offset=0;}
    else{index=id>>YH_COLD_SHIFT;*offset=id&YH_COLD_MASK;}
    if(!index||index>=yh_cold_count||!yh_cold_blocks[index])abort();
    yh_cold_block *block=yh_cold_blocks[index];
    if(*offset>=block->length)abort();
    return block;
}
static void yh_hist_cold_finish(void);
static void yh_hist_cold_audit(void);
void yh_hist_scope_enter(yh_hist_scope_T *scope)
{
    if(++yh_cold_scope_clock==0)abort();
    *scope=(yh_hist_scope_T){.id=yh_cold_scope_clock};
}
void yh_hist_scope_leave(yh_hist_scope_T *scope)
{
    yh_cold_pin *pin=scope->pinned;
    while(pin){
        yh_cold_pin *next=pin->next;yh_cold_block *block=pin->block;
        if(!block->pinned)abort();
        block->pinned--;
        if(!block->refs&&!block->pinned)yh_cold_release_block(block);
        free(pin);pin=next;
    }
    scope->pinned=NULL;scope->id=0;
    yh_hist_cold_finish();
    yh_hist_cold_audit();
}
static const char *yh_cold_text(yh_hist_scope_T *scope,uint32_t id)
{
    if(!scope->id)abort();
    size_t offset;yh_cold_block *block=yh_cold_resolve(id,&offset);
    if(block->last_scope!=scope->id){
        yh_cold_pin *pin=scope->pinned;
        while(pin&&pin->block!=block)pin=pin->next;
        if(!pin){
            pin=xmalloc(sizeof *pin);*pin=(yh_cold_pin){.block=block,.next=scope->pinned};
            scope->pinned=pin;block->pinned++;
        }
        block->last_scope=scope->id;
    }
    if(block->compressed&&!block->decoded){
        block->decoded=mmap(NULL,block->length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(block->decoded==MAP_FAILED){block->decoded=NULL;abort();}
        block->decoded_mapped=1;
        if(!yh_cold_decode(block->data,block->bytes,block->decoded,block->length)||
                yh_cold_checksum(block->decoded,block->length)!=block->checksum)abort();
    }
    block->stamp=++yh_cold_clock;
    const unsigned char *base=block->compressed?block->decoded:block->data;
    if(!memchr(base+offset,0,block->length-offset))abort();
    return (const char *)base+offset;
}
const char *yh_hist_value_scoped(yh_hist_scope_T *scope,const histentry_T *entry)
{
    return yh_cold_text(scope,entry->value_id);
}
static void yh_hist_drop(uint32_t id)
{
    size_t offset;yh_cold_block *block=yh_cold_resolve(id,&offset);
    /* Drop needs length accounting, not a permanent decoded allocation. */
    yh_hist_scope_T scope;yh_hist_scope_enter(&scope);
    const char *text=yh_cold_text(&scope,id);size_t length=add(strlen(text),1);
    if(!block->refs||length>block->live)abort();
    block->refs--;block->live-=length;
    yh_hist_scope_leave(&scope);
}
void yh_hist_cold_stats(size_t *encoded,size_t *decoded,size_t *live,size_t *blocks)
{
    *encoded=mul(yh_cold_capacity,sizeof *yh_cold_blocks);*decoded=*live=*blocks=0;
    for(size_t i=1;i<yh_cold_count;i++){
        yh_cold_block *block=yh_cold_blocks[i];if(!block)continue;
        *encoded=add(*encoded,add(sizeof *block,block->sealed?block->bytes:block->capacity));
        if(block->decoded)*decoded=add(*decoded,block->length);
        *live=add(*live,block->live);(*blocks)++;
    }
}
static void yh_hist_cold_finish(void)
{
    if(yh_cold_open)yh_cold_seal(yh_cold_open);
    yh_cold_trim();
}
static void yh_hist_cold_audit(void)
{
    const char *path=getenv("YASH_V5_AUDIT_FILE");if(!path||!*path)return;
    for(size_t i=1;i<yh_cold_count;i++)if(yh_cold_blocks[i]&&yh_cold_blocks[i]->pinned)return;
    size_t encoded,decoded,live,blocks;yh_hist_cold_stats(&encoded,&decoded,&live,&blocks);
    char *name=xmalloc(add(strlen(path),6));strcpy(name,path);strcat(name,".cold");
    FILE *file=fopen(name,"w");free(name);if(!file)return;
    fprintf(file,"{\"encoded_store_requested_bytes\":%zu,\"decoded_cache_bytes\":%zu,\"live_text_bytes\":%zu,\"blocks\":%zu,\"metadata_entry_bytes\":%zu,\"block_bytes\":%zu,\"cache_blocks\":%u,\"zlib\":%u}\n",
        encoded,decoded,live,blocks,sizeof(histentry_T),YH_COLD_BLOCK,(unsigned)YH_COLD_CACHE,(unsigned)YH_COLD_ZLIB);
    fclose(file);
}
#endif
