/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus YSF Voice FR transmit, end to end through the actual P25 channel worker (docs/ysf-nxdn-tx.md). Host only:
 * every RF edge is a fake, nothing transmits.
 *
 * The real p25_lab.c is #included (as tests/plus/test_ysf_worker.c does) and its worker task body run() is driven
 * with request_mode 4 (p25_lab_live_begin) on a YSF channel. Mocked: FreeRTOS and the clock, the board's PCM audio (a
 * microphone block every 20 ms of the virtual clock: tones and noise, logged as the worker's FIFO takes them), PTT,
 * and the native modem's live session (native_lab_live_*): it takes one 36-byte block every 30 ms from the moment the
 * worker says go, faults on an underrun (a block not nloaded by the time it is due) and stops once the sealed block has
 * gone. The YSF layer, the P25 IMBE FEC and the host IMBE codec are the real ones.
 *
 * The blocks the worker nloaded are then received by our own YSF layer: preamble, header (callsign, DG-ID),
 * sub-header, voice frames, terminator, and every IMBE frame equal to a fresh encoder's output for the microphone
 * blocks the worker took (then encoded silence up to the frame's end).
 *
 * Built twice by tools/test_classic.py: with CONFIG_YSF_TX=1 (all of the above), and with CONFIG_YSF_TX=0 (the
 * worker refuses a transmission on a YSF channel: no TX request without the switch). */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "dm32_imbe.h"
#if CONFIG_YSF_TX
static unsigned encode_cost_ms;	/* virtual time each IMBE encode takes (the radio: about 13 ms, at most 18.6) */
static int slow_encode(dm32_imbe_context *c,int16_t pcm[160],uint16_t u[8]);
#define dm32_imbe_encode slow_encode
#endif
#include "p25_lab.c"
#undef dm32_imbe_encode

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

#if CONFIG_YSF_TX
static int slow_encode(dm32_imbe_context *c,int16_t pcm[160],uint16_t u[8])
{
    clock_ms+=encode_cost_ms;
    return dm32_imbe_encode(c,pcm,u);
}
#endif

/* Microphone: a block every 20 ms while the worker's callback is installed; what the FIFO took is logged */
#define MAX_MIC 400
static audio_block_cb_t callback;
static void *callback_ctx;
static uint32_t audio_at,ptt_release_ms;
static unsigned audio_starts,audio_stops,route_on,route_calls,nmic;
static int16_t mic_log[MAX_MIC][160];
static uint32_t mic_rng=0x2468aceu;
static uint32_t mrnd(void) { mic_rng^=mic_rng<<13;mic_rng^=mic_rng>>17;mic_rng^=mic_rng<<5;return mic_rng; }
static bool audio_start(audio_block_cb_t cb,void *ctx)
{ assert(!callback);audio_starts++;callback=cb;callback_ctx=ctx;audio_at=clock_ms;return true; }
static void audio_stop(void) { audio_stops++;callback=NULL; }
static void mic_route(bool on) { route_on=on;route_calls++; }
static const hal_audio_t audio={.start=audio_start,.stop=audio_stop};
const board_t board={.audio=&audio,.native_mic_route=mic_route};
bool input_ptt_down(void) { return clock_ms<ptt_release_ms; }

/* ---- Native modem live session (fake) ---- */

#define MAX_BLOCKS 400
static uint8_t blocks[MAX_BLOCKS][36];
static unsigned nloaded,sealed_at,go_calls,stops,releases;
static uint32_t session_sid,go_ms,deadline_ms;
static bool kicked,faulted,sealed;
static int min_queue;
static uint8_t fault_reason;
static unsigned taken_by(uint32_t t) { return kicked && t>=go_ms ? (t-go_ms)/30u+1u : 0u; }
static void modem_update(void)
{
    if(!kicked || faulted)return;
    unsigned taken=taken_by(clock_ms);
    if(sealed && taken>=nloaded)return;		/* the whole call went out */
    if(taken>nloaded) { faulted=true;fault_reason=NT_UNDERRUN;return; }
    int q=(int)nloaded-(int)taken;
    if(!sealed && q<min_queue)min_queue=q;
}
int native_lab_live_poll(uint32_t sid,uint8_t *s,uint8_t *r,uint8_t *q)
{
    if(sid!=session_sid || stops)return 0;
    modem_update();
    unsigned taken=taken_by(clock_ms);
    if(faulted) { *s=NT_FAULT;*r=fault_reason; }
    else if(deadline_ms && clock_ms>=deadline_ms) { *s=NT_STOPPED;*r=NT_DEADLINE; }	/* the channel time-out */
    else if(kicked && sealed && taken>=nloaded) { *s=NT_STOPPED;*r=NT_TX_COMPLETE; }
    else *s=kicked?NT_TX:NT_ARMED,*r=NT_OK;
    unsigned in=nloaded>taken?nloaded-taken:0u;
    *q=(uint8_t)(in>255u?255u:in);
    return 1;
}
int native_lab_live_block(uint32_t sid,const uint8_t block[36],int last)
{
    if(sid!=session_sid || sealed || stops)return 0;
    modem_update();
    unsigned taken=taken_by(clock_ms);
    CHECK(nloaded-taken<NT_TX_CAP);			/* the ring holds eight */
    if(kicked && nloaded<taken) { faulted=true;fault_reason=NT_UNDERRUN; }	/* nloaded after it was due */
    assert(nloaded<MAX_BLOCKS);
    memcpy(blocks[nloaded++],block,36);
    if(last) { sealed=true;sealed_at=nloaded; }
    return 1;
}
int native_lab_live_go(uint32_t sid)
{
    if(sid!=session_sid || kicked)return 0;
    CHECK(nloaded>=2u);
    go_calls++;kicked=true;go_ms=clock_ms;min_queue=99;
    return 1;
}
void native_lab_live_summary(uint32_t out[5]) { memset(out,0,5*sizeof *out); }
void native_lab_voice_stop(uint32_t sid) { CHECK(sid==session_sid);stops++; }
int native_lab_voice_release(uint32_t sid) { CHECK(sid==session_sid && stops);releases++;return 1; }
int native_lab_reserved(void) { return 0; }
/* The receive side of the native session is not used here */
uint32_t native_lab_voice_start(uint32_t hz,uint32_t ms) { (void)hz;(void)ms;CHECK(!"receive started");return 0; }
int native_lab_voice_poll(uint32_t sid,uint8_t *s,uint8_t *why) { (void)sid;(void)s;(void)why;return 0; }
int native_lab_voice_read(uint32_t sid,nt_record *r) { (void)sid;(void)r;return 0; }
static void sk_begin(const dmr_caller_t *caller,void *ctx) { (void)caller;(void)ctx;CHECK(!"DMR voice"); }
static void sk_frame(const uint8_t a72[DMR_AMBE_FRAME_BYTES],void *ctx) { (void)a72;(void)ctx;CHECK(!"DMR voice"); }
static void sk_end(void *ctx) { (void)ctx; }
static const dmr_voice_sink_t mock_sink={.begin=sk_begin,.frame=sk_frame,.end=sk_end};
const dmr_voice_sink_t *ambeVoiceSink(void) { return &mock_sink; }
void ambeVoiceStop(void) { }

void vTaskDelay(TickType_t ms)
{
    assert(!depth);
    clock_ms+=ms;assert(clock_ms<=maximum_ms);
    while(callback && clock_ms-audio_at>=20u) {
        int16_t mic[160],spk[160];
        unsigned period=12u+mrnd()%60u,level=1000u+mrnd()%12000u;
        for(unsigned i=0;i<160u;i++) {
            unsigned phase=i%period;
            int tri=(int)(phase<period/2u?phase:period-phase)*(int)level*4/(int)period-(int)level;
            mic[i]=(int16_t)(tri+(int)(mrnd()%513u)-256);
        }
        uint32_t pushed=fifo.pushed;
        audio_at+=20;
        callback(mic,spk,callback_ctx);
        if(fifo.pushed!=pushed) { assert(nmic<MAX_MIC);memcpy(mic_log[nmic++],mic,sizeof mic); }
        modem_update();
    }
    modem_update();
}

/* ---- Running a transmission ---- */
#if CONFIG_YSF_TX

typedef struct {
    const char *what,*call;
    uint8_t dgid;
    uint32_t talk_ms,deadline_ms;
    unsigned cost_ms;
} tx_case;

static void start_case(const tx_case *c)
{
    clock_ms=depth=turn=0;
    callback=NULL;audio_starts=audio_stops=route_on=route_calls=nmic=0;
    nloaded=sealed_at=go_calls=stops=releases=0;go_ms=0;kicked=faulted=sealed=false;fault_reason=0;min_queue=99;
    session_sid++;
    deadline_ms=c->deadline_ms;encode_cost_ms=c->cost_ms;
    ptt_release_ms=c->talk_ms;
    maximum_ms=c->talk_ms+10000u;
    memset(blocks,0,sizeof blocks);
}

static unsigned getb(const uint8_t *p,unsigned i) { return (p[i>>3]>>(7u-(i&7u)))&1u; }

/* Receive the nloaded blocks with our YSF layer; the IMBE frames must be a fresh encoder's for mic_log, then silence */
static void receive_check(const tx_case *c,unsigned *imbe_out,unsigned *frames_out)
{
    static dm32_imbe_context ref;
    dm32_imbe_init(&ref);
    p25_mic_condition ref_cond;		/* the worker conditions the microphone as P25's live path does (Classic r8) */
    p25_mic_condition_init(&ref_cond);
    unsigned ref_done=0,imbe=0,frames=0,comms=0;
    bool header=false,terminator=false;
    char want_src[11];
    snprintf(want_src,sizeof want_src,"%-10s",c->call);
    for(unsigned b=0;b<3u && b<nloaded;b++)for(unsigned i=0;i<36u;i++)CHECK(blocks[b][i]==0x77);
    ysf_framer_t fr;
    ysf_framer_init(&fr);
    for(unsigned i=0;i<nloaded*288u;i++) {
        if(!ysf_framer_bit(&fr,(uint8_t)getb(blocks[i/288u],i%288u)))continue;
        ysf_fich_t f;
        uint8_t csd[YSF_CSD_BYTES];
        CHECK(ysf_fich_decode(fr.frame,&f) && f.dt==YSF_DT_VOICE_FR && f.dgid==c->dgid && !terminator);
        frames++;
        if(f.fi==YSF_FI_HEADER || f.fi==YSF_FI_TERMINATOR) {
            CHECK(f.fi==YSF_FI_HEADER ? !header && frames==1u : header);
            if(f.fi==YSF_FI_HEADER)header=true; else terminator=true;
            CHECK(ysf_csd_get(fr.frame,YSF_CSD1,csd) && !memcmp(csd+10,want_src,10) && !memcmp(csd,"**********",10));
            continue;
        }
        CHECK(header && f.fi==YSF_FI_COMMS && f.fn==0u && f.ft==(comms?0u:1u));
        comms++;
        for(unsigned k=0;k<ysf_vw_voice_count(&f);k++) {
            uint8_t cw[YSF_VW_IMBE_BYTES],u0e=9;
            uint16_t u[8],want[8];
            int16_t pcm[160]={0};
            CHECK(ysf_vw_voice(fr.frame,&f,k,cw) && p25_imbe_decode(cw,u,&u0e)==0 && u0e==0);
            if(ref_done<nmic) {		/* past the microphone: silence (not conditioned) */
                memcpy(pcm,mic_log[ref_done],sizeof pcm);
                p25_mic_condition_block(&ref_cond,pcm,0);
            }
            ref_done++;
            CHECK(dm32_imbe_encode(&ref,pcm,want));
            want[7]=(uint16_t)((want[7]&~1u)|(imbe&1u));
            CHECK(!memcmp(u,want,sizeof u));
            imbe++;
        }
    }
    CHECK(header && terminator);
    CHECK(imbe>=nmic && imbe<nmic+5u);			/* every block taken went out, then at most 4 of silence */
    *imbe_out=imbe;*frames_out=frames;
}

static void run_case(const tx_case *c)
{
    unsigned before=failures;
    start_case(c);
    CHECK(p25_lab_set_ysf(1,c->dgid));
    CHECK(p25_lab_set_ysf_tx(c->call));
    CHECK(p25_lab_live_begin(session_sid));
    if(!setjmp(finished))worker_fn(NULL);
    CHECK(!depth && !callback && audio_starts==1 && audio_stops>=1 && !route_on && route_calls==2);
    CHECK(stops==1 && releases==1 && !fifo.overflows);
    unsigned imbe=0,frames=0;
    if(c->deadline_ms) {
        /* The channel time-out stopped the modem: the worker gives up at once (failed run), nothing more nloaded */
        CHECK(state==3 && kicked && !sealed);
        printf("%s %s: stopped at %u ms with %u blocks\n",failures==before?"ok  ":"FAIL",c->what,
               (unsigned)clock_ms,nloaded);
        return;
    }
    CHECK(state==2 && kicked && go_calls==1 && sealed && sealed_at==nloaded && !faulted);
    for(unsigned i=0;i<36u;i++)CHECK(blocks[nloaded-1u][i]==0x77);	/* the sealing block */
    receive_check(c,&imbe,&frames);
    /* 3 preamble blocks + the frames + the sealing block, the terminator's block filled up */
    CHECK(nloaded==3u+(frames*YSF_FRAME_BITS+287u)/288u+1u);
    CHECK(live_encoded==imbe && live_mic_frames==nmic);
    printf("%s %s: %u mic blocks, %u IMBE frames, %u frames, %u blocks, least queued %d, encode max %u us\n",
           failures==before?"ok  ":"FAIL",c->what,nmic,imbe,frames,nloaded,min_queue,(unsigned)live_encode_max);
}

/* An empty PTT (released before any speech): the preamble and header are loaded but the modem is never started */
static void test_empty(void)
{
    unsigned before=failures;
    tx_case c={"empty PTT","TA1ABC",0,0,0,0};
    start_case(&c);
    CHECK(p25_lab_set_ysf(1,0) && p25_lab_set_ysf_tx("TA1ABC") && p25_lab_live_begin(session_sid));
    if(!setjmp(finished))worker_fn(NULL);
    CHECK(state==2 && !kicked && !go_calls && !sealed && stops==1 && releases==1 && !route_on && !nmic);
    printf("%s empty PTT: nothing keyed (%u blocks loaded, never started)\n",failures==before?"ok  ":"FAIL",nloaded);
}

/* The worker refuses: no callsign; and on a P25 channel the P25 path still runs (not the YSF one) */
static void test_refusals(void)
{
    unsigned before=failures;
    tx_case c={"refusals","",0,1000,0,0};
    start_case(&c);
    CHECK(p25_lab_set_ysf(1,0));
    p25_lab_set_ysf_tx("");
    CHECK(!p25_lab_live_begin(session_sid) && state!=1);
    p25_lab_set_ysf_tx("TA1ABCDEFGHIJ");			/* cut to 10 characters */
    CHECK(!strcmp(ysf_tx_call,"TA1ABCDEFG"));
    printf("%s no callsign: refused\n",failures==before?"ok  ":"FAIL");
}
#endif

int main(void)
{
    p25_lab_start();beep_init(&audio);
#if CONFIG_YSF_TX
    static const tx_case cases[]={
        {"2 s call, DG-ID 0","TA1ABC",0,2000,0,0},
        {"5 s call, DG-ID 42, encoder at 13 ms","TA2XYZ",42,5000,0,13},
        {"5 s call, encoder at its slowest 18 ms","TA1ABC",127,5000,0,18},
        {"short call (0.1 s)","TA1ABC",1,100,0,13},
        {"10-character callsign","TA1ABCDEFG",5,1300,0,5},
        {"channel time-out at 1.5 s","TA1ABC",0,4000,1500,13},
    };
    test_refusals();
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];i++)run_case(&cases[i]);
    test_empty();
#else
    /* No YSF transmitter in this build: a YSF channel's PTT never reaches the modem */
    CHECK(p25_lab_set_ysf(1,0));
    CHECK(!p25_lab_live_begin(1) && state!=1);
    puts(failures?"FAIL no YSF transmitter: refused":"ok   no YSF transmitter: the worker refuses a YSF transmission");
#endif
    printf("ysf tx worker (CONFIG_YSF_TX=%d): %u failures\n",CONFIG_YSF_TX,failures);
    return failures?1:0;
}
