/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM25 Plus separate RX and TX NAC (Classic r13 split NAC, CONFIG_P25_TXNAC, docs/plus.md "Separate RX NAC and TX NAC"), end to end through
 * the actual P25 channel worker. Host only: every RF edge is a fake, nothing transmits.
 *
 * The real p25_lab.c is #included (as tests/plus/test_ysf_tx_worker.c does) and its worker task body run() is driven
 * with request_mode 4 (p25_lab_live_begin) on a P25 channel armed the way native_lab.c arms it: Classic r13's
 * p25_lab_set_channel with the RX NAC, the effective TX NAC (codeplug_p25_tx_nac), the talkgroup, the unit ID and the
 * squelch. Mocked: FreeRTOS and the clock, the board's PCM audio (a microphone block
 * every 20 ms), PTT, and the native modem's live session (one 36-byte block every 30 ms once the worker says go).
 *
 * Every block the worker loaded is then received by our own P25 framer with any NAC: every frame's NID (HDU, LDU1,
 * LDU2, the terminator) carries the TX NAC, never the RX NAC, and the LDU1 link control the talkgroup and the unit ID.
 * The receive side (p25_lab_channel_rx) still takes the RX NAC as its filter, and the talkgroup filter only with the
 * Selective squelch. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "p25_lab.c"

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
static uint32_t audio_at,ptt_release_ms;
static uint32_t mic_rng=0x2468aceu;
static uint32_t mrnd(void) { mic_rng^=mic_rng<<13;mic_rng^=mic_rng>>17;mic_rng^=mic_rng<<5;return mic_rng; }
static bool audio_start(audio_block_cb_t cb,void *ctx) { assert(!callback);callback=cb;callback_ctx=ctx;audio_at=clock_ms;return true; }
static void audio_stop(void) { callback=NULL; }
static void mic_route(bool on) { (void)on; }
static const hal_audio_t audio={.start=audio_start,.stop=audio_stop};
const board_t board={.audio=&audio,.native_mic_route=mic_route};
bool input_ptt_down(void) { return clock_ms<ptt_release_ms; }

/* ---- Native modem live session (fake) ---- */

#define MAX_BLOCKS 300
static uint8_t blocks[MAX_BLOCKS][36];
static unsigned nloaded,stops,releases;
static uint32_t session_sid,go_ms;
static bool kicked,sealed;
static unsigned taken_by(uint32_t t) { return kicked && t>=go_ms ? (t-go_ms)/30u+1u : 0u; }
int native_lab_live_poll(uint32_t sid,uint8_t *s,uint8_t *r,uint8_t *q)
{
    if(sid!=session_sid || stops)return 0;
    unsigned taken=taken_by(clock_ms);
    if(kicked && sealed && taken>=nloaded) { *s=NT_STOPPED;*r=NT_TX_COMPLETE; }
    else *s=kicked?NT_TX:NT_ARMED,*r=NT_OK;
    unsigned in=nloaded>taken?nloaded-taken:0u;
    *q=(uint8_t)(in>255u?255u:in);
    return 1;
}
int native_lab_live_block(uint32_t sid,const uint8_t block[36],int last)
{
    if(sid!=session_sid || sealed || stops)return 0;
    assert(nloaded<MAX_BLOCKS);
    memcpy(blocks[nloaded++],block,36);
    if(last)sealed=true;
    return 1;
}
int native_lab_live_go(uint32_t sid) { if(sid!=session_sid || kicked)return 0;kicked=true;go_ms=clock_ms;return 1; }
void native_lab_live_summary(uint32_t out[5]) { memset(out,0,5*sizeof *out); }
void native_lab_voice_stop(uint32_t sid) { CHECK(sid==session_sid);stops++; }
int native_lab_voice_release(uint32_t sid) { CHECK(sid==session_sid && stops);releases++;return 1; }
int native_lab_reserved(void) { return 0; }
uint32_t native_lab_voice_start(uint32_t hz,uint32_t ms) { (void)hz;(void)ms;CHECK(!"receive started");return 0; }
int native_lab_voice_poll(uint32_t sid,uint8_t *s,uint8_t *why) { (void)sid;(void)s;(void)why;return 0; }
int native_lab_voice_read(uint32_t sid,nt_record *r) { (void)sid;(void)r;return 0; }

void vTaskDelay(TickType_t ms)
{
    assert(!depth);
    clock_ms+=ms;assert(clock_ms<=maximum_ms);
    while(callback && clock_ms-audio_at>=20u) {
        int16_t mic[160],spk[160];
        for(unsigned i=0;i<160u;i++)mic[i]=(int16_t)((int)(mrnd()%8001u)-4000);
        audio_at+=20;
        callback(mic,spk,callback_ctx);
    }
}

/* ---- One transmission, received back ---- */

typedef struct { const char *what; uint16_t rx,tx; bool selective; uint16_t want; } nac_case;

static void run_case(const nac_case *c)
{
    unsigned before=failures;
    clock_ms=depth=turn=0;callback=NULL;
    nloaded=stops=releases=0;go_ms=0;kicked=sealed=false;
    session_sid++;ptt_release_ms=1500;maximum_ms=12000;
    memset(blocks,0,sizeof blocks);
    CHECK(p25_lab_set_channel(c->rx,c->tx,101,1234567,c->selective));
    CHECK(p25_lab_live_begin(session_sid));
    if(!setjmp(finished))worker_fn(NULL);
    CHECK(state==2 && kicked && sealed && stops==1 && releases==1);
    /* Received with any NAC: every frame's NID */
    p25_rx rx;
    p25_rx_init(&rx,P25_NAC_ANY,0);
    unsigned frames=0,wrong=0,hdu=0,ldu1=0,ldu2=0,term=0,lc_ok=0;
    for(unsigned b=0;b<nloaded;b++)
        for(unsigned i=0;i<144u;i++) {
            if(!p25_rx_push_dibit(&rx,(uint8_t)((blocks[b][i/4u]>>(6u-(i%4u)*2u))&3u)))continue;
            frames++;
            if(rx.nid.nac!=c->want)wrong++;
            switch(rx.nid.duid) {
            case P25_DUID_HDU: hdu++;break;
            case P25_DUID_LDU1: {
                ldu1++;
                p25_decoded d;
                if(p25_decode_frame(rx.frame,p25_frame_bytes(rx.nid.duid),&d)==0 && d.metadata_valid &&
                   d.nid.nac==c->want && d.talkgroup==101 && d.source_id==1234567u)lc_ok++;
                break;
            }
            case P25_DUID_LDU2: ldu2++;break;
            default: term++;break;
            }
        }
    CHECK(frames>=4 && !wrong && hdu==1 && ldu1>=1 && ldu2>=1 && term>=1 && lc_ok==ldu1);
    /* The receive filter: the RX NAC (Any as P25_NAC_ANY), whatever the TX NAC */
    CHECK(p25_lab_channel_rx(433500000u));
    CHECK(nac==(c->rx==0xF7Eu?P25_NAC_ANY:c->rx) && group==(c->selective?101:0));
    state=0;request_mode=0;play_requested=0;
    printf("%s %s: %u frames (HDU %u, LDU1 %u, LDU2 %u, end %u) all NAC $%03X; receive filter %s\n",
           failures==before?"ok  ":"FAIL",c->what,frames,hdu,ldu1,ldu2,term,c->want,
           c->rx==0xF7Eu?"any":"the RX NAC");
}

int main(void)
{
    p25_lab_start();beep_init(&audio);
    static const nac_case cases[]={
        {"RX $293, TX $659 (a repeater)",0x293,0x659,true,0x659},
        {"RX Any, TX $659",0xF7E,0x659,true,0x659},
        {"RX $293, TX the same",0x293,0x293,true,0x293},
        {"RX Any, TX inherited ($293), Normal squelch",0xF7E,0x293,false,0x293},
        {"RX $125, TX $000",0x125,0x000,true,0x000},
    };
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];i++)run_case(&cases[i]);
    /* Not while the worker runs: refused, as p25_lab_set_channel is */
    state=1;
    CHECK(!p25_lab_set_channel(0x111,0x111,1,1,1));
    state=0;
    printf("p25nac tx worker: %u failures\n",failures);
    return failures?1:0;
}
