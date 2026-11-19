/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 trunk following, end to end through the actual P25 channel worker (built with CONFIG_P25_TRUNK=1).
 *
 * As tests/plus/test_ysf_worker.c does, the real p25_lab.c is #included and its worker task body (receive_run,
 * request_mode 5) is driven with p25_lab_set_trunk(1). Only the edges are mocked: FreeRTOS and the clock, the native
 * modem session (one 36-byte RAM record every 30 ms) and the board's PCM audio. The PHY record mapping
 * (src/p25/phy_stream.c), the framer and voice receive (src/p25/p25.c, voice.c), the TSBK decoder
 * (trunk/p25_tsbk.c) and the host IMBE codec are the real ones.
 *
 * A control channel stream is built with the test's own TSDU encoder (p25_tsdu_build.h): one-, two- and three-block
 * TSDUs, one block damaged beyond repair, then a voice call (HDU, LDU1, LDU2, TDU) as on a voice channel. It is cut
 * into 288-bit records, mapped to the hardware RAM layout with the inverse of p25_phy_rx_air(), and played to the
 * worker. Every valid TSBK must come out of p25_lab_tsbk_pop() in order, the damaged one counted as bad, the voice
 * frames and the terminator counted for the follower; without trunk mode the same stream yields no TSBK at all. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "p25_lab.c"
#include "p25_tsdu_build.h"

static unsigned failures;
#define CHECK(c) do { if(!(c)) { failures++; printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); } } while(0)

/* ---- FreeRTOS, clock, board ---- */

static jmp_buf finished;
static void (*worker_fn)(void *);
static uint32_t clock_ms,maximum_ms;
static unsigned depth,turn;

void audio_test_enter(void) { depth++; }
void audio_test_exit(void) { assert(depth);depth--; }
uint32_t hal_millis(void) { return clock_ms; }
uint32_t hal_micros(void) { return clock_ms*1000u; }
BaseType_t xTaskCreate(void (*fn)(void *),const char *name,unsigned words,void *ctx,unsigned priority,TaskHandle_t *h)
{ (void)name;(void)words;(void)ctx;(void)priority;worker_fn=fn;*h=(void *)1;return pdPASS; }
void xTaskNotifyGive(TaskHandle_t h) { assert(h && !depth); }
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t wait)
{ assert(clear && wait==portMAX_DELAY);if(turn++)longjmp(finished,1);return 1; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t h) { (void)h;return 512; }

static audio_block_cb_t callback;
static void *callback_ctx;
static uint32_t audio_at;
static bool audio_start(audio_block_cb_t cb,void *ctx) { callback=cb;callback_ctx=ctx;audio_at=clock_ms;return true; }
static void audio_stop(void) { callback=NULL; }
static void route(bool on) { (void)on; }
static const hal_audio_t audio={.start=audio_start,.stop=audio_stop,.speaker_pcm=route};
const board_t board={.audio=&audio};

/* What the radio task would see: TSBKs taken while the worker runs (a few at a time, as native_lab.c does) */
static uint8_t popped[32][10];
static unsigned npopped;
void vTaskDelay(TickType_t ms)
{
    assert(!depth);
    clock_ms+=ms;assert(clock_ms<=maximum_ms);
    if(clock_ms%50u==0) { uint8_t b[10];while(npopped<32u && p25_lab_tsbk_pop(b))memcpy(popped[npopped++],b,10); }
    while(callback && clock_ms-audio_at>=20u) {
        int16_t mic[160]={0},spk[160];
        audio_at+=20;
        callback(mic,spk,callback_ctx);
    }
}
bool input_ptt_down(void) { return false; }
int native_lab_reserved(void) { return 0; }
int native_lab_live_poll(uint32_t sid,uint8_t *s,uint8_t *r,uint8_t *q)
{ (void)sid;(void)s;(void)r;(void)q;assert(0);return 0; }
int native_lab_live_block(uint32_t sid,const uint8_t block[36],int last)
{ (void)sid;(void)block;(void)last;assert(0);return 0; }
int native_lab_live_go(uint32_t sid) { (void)sid;assert(0);return 0; }
void native_lab_live_summary(uint32_t out[5]) { memset(out,0,5*sizeof *out); }

/* ---- Native modem session: one record every 30 ms, then empty (all-zero) records ---- */

#define MAX_RECORDS 120
static uint8_t rec_raw[MAX_RECORDS][36];
static unsigned rec_count,rec_next,starts,owned;
static uint32_t next_raw_at;
uint32_t native_lab_voice_start(uint32_t hz,uint32_t ms)
{ assert(hz==433500000u && !ms && !owned);owned=1;starts++;next_raw_at=0;rec_next=0;return starts; }
int native_lab_voice_poll(uint32_t sid,uint8_t *s,uint8_t *why)
{ assert(sid==starts && owned);*s=NT_RX;*why=NT_OK;return 1; }
int native_lab_voice_read(uint32_t sid,nt_record *r)
{
    assert(sid==starts && owned);
    if(clock_ms<next_raw_at)return 0;
    memset(r,0,sizeof *r);r->sequence=rec_next;r->time_us=next_raw_at*1000u;
    if(rec_next<rec_count)memcpy(r->raw,rec_raw[rec_next],36);
    rec_next++;next_raw_at+=30;return 1;
}
void native_lab_voice_stop(uint32_t sid) { assert(sid==starts && owned); }
int native_lab_voice_release(uint32_t sid) { assert(sid==starts && owned);owned=0;return 1; }

/* ---- Air stream ---- */

static uint32_t rng=0x13579BDFu;
static uint32_t rnd(void) { rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng; }
#define MAX_STREAM_BITS (MAX_RECORDS*288u)
static uint8_t stream[MAX_STREAM_BITS/8];
static unsigned stream_bits;
static void add_bit(unsigned b) { assert(stream_bits<MAX_STREAM_BITS);tsdu_putb(stream,stream_bits++,b); }
static void add_noise(unsigned dibits) { while(dibits--) { add_bit(rnd()&1u);add_bit(rnd()&1u); } }
static void add_bits(const uint8_t *p,unsigned bits) { for(unsigned i=0;i<bits;i++)add_bit(tsdu_getb(p,i)); }

static uint8_t sent[16][12];
static unsigned nsent;
static void block(uint8_t b[12],uint8_t op,bool last,const uint8_t args[8])
{
    b[0]=(uint8_t)((last?0x80u:0)|op);b[1]=0;memcpy(b+2,args,8);tsbk_seal(b);
}
/* A TSDU of n blocks; damaged: its block 0 is hit by noise (the TSDU is dropped: its blocks are not expected out) */
static void add_tsdu(const uint8_t blocks[][12],unsigned n,bool damaged)
{
    uint8_t frame[TSDU_MAX_BYTES];
    unsigned dibits=tsdu_build(frame,0x1C8,blocks,n);
    if(damaged)for(unsigned i=0;i<40u;i++) {
        unsigned p=tsdu_physical(112u+rnd()%196u);
        frame[p>>3]^=(uint8_t)(0x80u>>(p&7u));
    }
    add_bits(frame,2u*dibits);
    /* A control channel pads every TSDU to its microslot boundary (null fill): the CAI library's control receiver
     * completes a TSDU there */
    add_noise(4u*(unsigned)p25cai_control_size(P25CAI_TSDU,n)-dibits);
    if(!damaged)for(unsigned k=0;k<n;k++)memcpy(sent[nsent++],blocks[k],12);
}

static const uint16_t layout01[][2]={{0,24},{72,108},{24,48},{180,108}};
static void air_to_ram(const uint8_t air[36],uint8_t ram[36])
{
    unsigned a=0;
    memset(ram,0,36);
    for(unsigned s=0;s<4u;s++)for(unsigned i=0;i<layout01[s][1];i++,a++)tsdu_putb(ram,layout01[s][0]+i,tsdu_getb(air,a));
}
static void make_records(void)
{
    rec_count=(stream_bits+287u)/288u;assert(rec_count<=MAX_RECORDS);
    for(unsigned r=0;r<rec_count;r++) {
        uint8_t air[36]={0},back[36];
        for(unsigned i=0;i<288u;i++)if(r*288u+i<stream_bits)tsdu_putb(air,i,tsdu_getb(stream,r*288u+i));
        air_to_ram(air,rec_raw[r]);
        CHECK(!p25_phy_rx_air(rec_raw[r],0,back) && !memcmp(back,air,36));
    }
}

static void build_stream(void)
{
    static const uint8_t iden_up[8]={0x13,0x20,0x00,100,0x05,0x29,0x69,0x40};	/* id 1, 12.5 kHz from 433.000 MHz */
    static const uint8_t net[8]={0x01,0xBE,0xE0,0x01,0xC8,0x10,0x01,0x70};
    static const uint8_t rfss[8]={0x01,0x11,0xC8,0x01,0x05,0x10,0x01,0x70};
    static const uint8_t grant[8]={0x00,0x10,80,0x00,101,0x12,0xD6,0x87};			/* TG 101, src 1234567 */
    static const uint8_t update[8]={0x10,80,0x00,101,0x10,80,0x00,101};
    static const uint8_t iden_vu[8]={0x35,0x80,0x00,100,0x05,0x2A,0x50,0x00};
    uint8_t b[3][12];
    memset(stream,0,sizeof stream);stream_bits=0;nsent=0;
    add_noise(77);
    block(b[0],0x3D,true,iden_up);add_tsdu(b,1,false);
    add_noise(3);
    block(b[0],0x3B,false,net);block(b[1],0x3A,false,rfss);block(b[2],0x00,true,grant);add_tsdu(b,3,false);
    block(b[0],0x02,true,update);add_tsdu(b,1,true);	/* damaged */
    block(b[0],0x02,false,update);block(b[1],0x34,true,iden_vu);add_tsdu(b,2,false);
    add_noise(1);
    /* A call as a voice channel carries it: HDU, LDU1, LDU2, TDU */
    const p25_call call={.nac=0x1C8,.talkgroup=101,.source_id=1234567};
    uint8_t frame[P25_LDU_BYTES];
    uint16_t imbe[P25_IMBE_FRAMES][8];
    const uint8_t lsd[2]={0,0};
    memset(imbe,0,sizeof imbe);
    CHECK(!p25_encode_hdu(&call,frame));add_bits(frame,8u*P25_HDU_BYTES);
    CHECK(!p25_encode_ldu(&call,P25_DUID_LDU1,imbe,lsd,frame));add_bits(frame,8u*P25_LDU_BYTES);
    CHECK(!p25_encode_ldu(&call,P25_DUID_LDU2,imbe,lsd,frame));add_bits(frame,8u*P25_LDU_BYTES);
    CHECK(!p25_encode_tdu(&call,frame));add_bits(frame,8u*P25_TDU_BYTES);
    add_noise(40);
    make_records();
}

static void execute(bool trunk)
{
    clock_ms=depth=turn=0;npopped=0;callback=NULL;
    maximum_ms=rec_count*30u+3000u;
    CHECK(p25_lab_set_channel(0x1C8,0x1C8,0,0,0));
    if(trunk)CHECK(p25_lab_set_trunk(1));
    CHECK(p25_lab_channel_rx(433500000u));
    if(!setjmp(finished))worker_fn(NULL);
    CHECK(state==2 && starts && !owned && !depth);
    uint8_t b[10];
    while(npopped<32u && p25_lab_tsbk_pop(b))memcpy(popped[npopped++],b,10);
}

int main(void)
{
    p25_lab_start();beep_init(&audio);
    build_stream();
    p25_lab_trunk_t c0,c1;
    p25_lab_trunk_counts(&c0);
    execute(true);
    p25_lab_trunk_counts(&c1);
    CHECK(npopped==nsent && nsent==6u);
    for(unsigned i=0;i<npopped && i<nsent;i++)CHECK(!memcmp(popped[i],sent[i],10));
    CHECK(c1.bad-c0.bad==1u && c1.dropped==c0.dropped);
    CHECK(c1.frames-c0.frames==10u && c1.terms-c0.terms==1u && c1.nac==0x1C8u);
    CHECK(voice.rx_frames==10u && !unsupported);
    printf("%s trunk worker: %u TSBKs out of 1-, 3- and 2-block TSDUs in order, the damaged block counted bad, "
           "HDU/LDU1/LDU2/TDU counted (frames %u, terminators %u)\n",failures?"FAIL":"ok  ",npopped,
           (unsigned)(c1.frames-c0.frames),(unsigned)(c1.terms-c0.terms));
    /* A conventional channel receive (set_channel turns trunk mode off): no TSDU is framed, nothing is counted */
    unsigned before=failures;
    execute(false);
    p25_lab_trunk_counts(&c0);
    CHECK(npopped==0 && c0.frames==c1.frames && c0.bad==c1.bad && voice.framer.bad_nids>=4u && voice.rx_frames==4u);
    printf("%s conventional receive: TSDUs not framed (bad NIDs %u), the call still heard\n",
           failures==before?"ok  ":"FAIL",(unsigned)voice.framer.bad_nids);
    printf("trunk worker: %u failures\n",failures);
    return failures?1:0;
}
