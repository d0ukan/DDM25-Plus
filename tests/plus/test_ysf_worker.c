/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus YSF receive, end to end through the actual P25 channel worker (built with CONFIG_YSF=1).
 *
 * The real p25_lab.c is #included (as tests/audio/test_receive_idle.c does) so the test drives the real worker task
 * body, receive_run(), with request_mode 5 and p25_lab_set_ysf(1, dgid). Only the edges are mocked: FreeRTOS and the
 * clock, the native modem session (native_lab_voice_*: one 36-byte RAM record every 30 ms), the DMR vocoder's
 * voice sink (ambeVoiceSink / ambeVoiceStop, which record begin / frame / end), and the board's PCM audio (its block
 * callback is run every 20 ms and every block popped from the worker's PCM FIFO is captured). The YSF layer
 * (src/ysf/ysf.c, ysf_vw.c), the PHY record mapping (src/p25/phy_stream.c), the P25 IMBE FEC (src/p25/p25.c), the
 * host IMBE codec (libdm32_imbe.a) and RadioFoundry's ambeFec.c are the real ones.
 *
 * Frames are built with the YSF encoders (ysf_fich_encode, ysf_vd2_data_put, ysf_vd2_voice_put, ysf_vw_voice_put),
 * concatenated into one air bit stream, cut into 288-bit records and mapped to hardware RAM layout with the inverse
 * of p25_phy_rx_air() (written here from its segment table and checked against it). Each AMBE frame the sink
 * receives must equal ambeFecEncode() of the 49 parameter bits that were put in. Voice FR (VW) frames carry IMBE
 * parameters that the host codec's encoder made from varying tones (so they are valid codec input), under
 * p25_imbe_encode(); every PCM block the worker plays must equal a reference dm32_imbe_decode() of them. */
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

/* Board PCM audio: DN leaves it to the DMR vocoder (untouched); VW plays on it */
#define MAX_PCM 96
static audio_block_cb_t callback;
static void *callback_ctx;
static uint32_t audio_at;
static unsigned audio_starts,audio_stops,route_calls,nplayed;
static int16_t played_pcm[MAX_PCM][160];
static bool audio_start(audio_block_cb_t cb,void *ctx)
{ assert(!callback);audio_starts++;callback=cb;callback_ctx=ctx;audio_at=clock_ms;return true; }
static void audio_stop(void) { audio_stops++;callback=NULL; }
static void route(bool on) { (void)on;route_calls++; }
static const hal_audio_t audio={.start=audio_start,.stop=audio_stop,.speaker_pcm=route};
const board_t board={.audio=&audio};

static unsigned receiving_ticks,frame_events,caller_after_end;
static uint32_t last_frame_ms;
static unsigned nev_end(void);
void vTaskDelay(TickType_t ms)
{
    assert(!depth);
    clock_ms+=ms;assert(clock_ms<=maximum_ms);
    /* The UI's "receiving" indicator follows YSF voice */
    if(frame_events && clock_ms-last_frame_ms<200u && p25_lab_channel_receiving())receiving_ticks++;
    /* The caller as the radio task sees it, once the sink saw the call end */
    if(nev_end()) {
        char src[11],dst[11];
        p25_lab_ysf_caller(src,dst);
        if(src[0] || dst[0])caller_after_end++;
    }
    while(callback && clock_ms-audio_at>=20u) {
        int16_t mic[160]={0},spk[160];
        uint32_t popped=fifo.popped;
        audio_at+=20;
        callback(mic,spk,callback_ctx);
        if(fifo.popped!=popped) { assert(nplayed<MAX_PCM);memcpy(played_pcm[nplayed++],spk,sizeof spk); }
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

#define MAX_RECORDS 200
#define LOST 0xFFu	/* rec_kind: the modem never delivers this record (its 30 ms slot passes) */
static uint8_t rec_raw[MAX_RECORDS][36],rec_kind[MAX_RECORDS];
static unsigned rec_count,rec_next,starts,owned;
static uint32_t next_raw_at,stream_end_ms;

uint32_t native_lab_voice_start(uint32_t hz,uint32_t ms)
{ assert(hz==439000000 && !ms && !owned);owned=1;starts++;next_raw_at=0;rec_next=0;return starts; }
int native_lab_voice_poll(uint32_t sid,uint8_t *s,uint8_t *why)
{ assert(sid==starts && owned);*s=NT_RX;*why=NT_OK;return 1; }
int native_lab_voice_read(uint32_t sid,nt_record *r)
{
    assert(sid==starts && owned);
    if(clock_ms<next_raw_at)return 0;
    memset(r,0,sizeof *r);r->sequence=rec_next;r->time_us=next_raw_at*1000u;
    if(rec_next<rec_count && rec_kind[rec_next]==LOST) { rec_next++;next_raw_at+=30;r->time_us+=30000u; }
    if(rec_next<rec_count) {
        memcpy(r->raw,rec_raw[rec_next],36);r->rx_info=rec_kind[rec_next];
        if(rec_next+1==rec_count)stream_end_ms=clock_ms;
    }
    rec_next++;next_raw_at+=30;return 1;
}
void native_lab_voice_stop(uint32_t sid) { assert(sid==starts && owned); }
int native_lab_voice_release(uint32_t sid) { assert(sid==starts && owned);owned=0;return 1; }

/* ---- DMR voice sink mock ---- */

enum { EV_BEGIN, EV_FRAME, EV_END };
#define MAX_EVENTS 600
static struct { uint8_t type,a72[AMBE_FRAME_BYTES];uint32_t at; } ev[MAX_EVENTS];
static unsigned nev,voice_stops;
static uint32_t voice_stop_at;
static int sink_tag;
static char seen_src[11],seen_dst[11];
static void push_event(uint8_t type,const uint8_t *a72,void *ctx)
{
    CHECK(ctx==&sink_tag);
    assert(nev<MAX_EVENTS);
    ev[nev].type=type;ev[nev].at=clock_ms;
    if(a72)memcpy(ev[nev].a72,a72,AMBE_FRAME_BYTES);
    nev++;
}
static void sk_begin(const dmr_caller_t *caller,void *ctx) { CHECK(caller==NULL);push_event(EV_BEGIN,NULL,ctx); }
static void sk_frame(const uint8_t a72[DMR_AMBE_FRAME_BYTES],void *ctx)
{
    push_event(EV_FRAME,a72,ctx);frame_events++;last_frame_ms=clock_ms;
    p25_lab_ysf_caller(seen_src,seen_dst);	/* the callsigns while the call plays */
}
static void sk_lost(void *ctx) { (void)ctx;CHECK(!"lost() is never used by YSF"); }
static void sk_end(void *ctx) { push_event(EV_END,NULL,ctx); }
static const dmr_voice_sink_t mock_sink={.begin=sk_begin,.frame=sk_frame,.lost=sk_lost,.end=sk_end,.ctx=&sink_tag};
const dmr_voice_sink_t *ambeVoiceSink(void) { return &mock_sink; }
void ambeVoiceStop(void) { voice_stops++;voice_stop_at=clock_ms; }
static unsigned nev_end(void) { return nev && ev[nev-1].type==EV_END; }

/* ---- Air stream builder ---- */

static uint32_t rng=0x12345678u;
static uint32_t rnd(void) { rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng; }
static unsigned getb(const uint8_t *p,unsigned i) { return (p[i>>3]>>(7u-(i&7u)))&1u; }
static void putb(uint8_t *p,unsigned i,unsigned b)
{ uint8_t m=(uint8_t)(0x80u>>(i&7u));p[i>>3]=b?(uint8_t)(p[i>>3]|m):(uint8_t)(p[i>>3]&~m); }
static void flipb(uint8_t *p,unsigned i) { p[i>>3]^=(uint8_t)(0x80u>>(i&7u)); }

#define MAX_STREAM_BITS (MAX_RECORDS*288u)
static uint8_t stream[MAX_STREAM_BITS/8];
static unsigned stream_bits;
/* Expected sink output: the AMBE frames of accepted YSF frames, and where calls begin / end */
static struct { uint8_t type,a72[AMBE_FRAME_BYTES]; } want[MAX_EVENTS];
static unsigned nwant;

static void reset_stream_builder(void) { memset(stream,0,sizeof stream);stream_bits=0;nwant=0; }
static void add_bit(unsigned b) { assert(stream_bits<MAX_STREAM_BITS);putb(stream,stream_bits++,b); }
static void add_noise(unsigned n) { while(n--)add_bit(rnd()&1u); }
static void add_frame(const uint8_t frame[YSF_FRAME_BYTES]) { for(unsigned i=0;i<YSF_FRAME_BITS;i++)add_bit(getb(frame,i)); }
static void want_event(uint8_t type,const uint8_t *a72)
{ assert(nwant<MAX_EVENTS);want[nwant].type=type;if(a72)memcpy(want[nwant].a72,a72,AMBE_FRAME_BYTES);nwant++; }

/* The V/D mode 2 data channel: FN 1 carries the source callsign, FN 0 the destination */
#define SRC_CALL "TA1SRC"
#define DST_CALL "TA1DST"
/* One YSF frame. fi/dt/dgid/fn/ft go into the FICH; V/D mode 2 voice channels get random parameter bits (returned) */
static void make_frame_ft(uint8_t fi,uint8_t dt,uint8_t dgid,uint8_t fn,uint8_t ft,uint8_t frame[YSF_FRAME_BYTES],
                          uint8_t params[YSF_VCH_PER_FRAME][YSF_AMBE_BITS],ysf_fich_t *f)
{
    for(unsigned i=0;i<YSF_FRAME_BYTES;i++)frame[i]=(uint8_t)rnd();
    const uint8_t info[4]={(uint8_t)(fi<<6),(uint8_t)(((fn&7u)<<3)|(ft&7u)),(uint8_t)(dt&3u),(uint8_t)(dgid&0x7Fu)};
    ysf_fich_encode(info,frame);
    CHECK(ysf_fich_decode(frame,f) && f->fi==fi && f->dt==dt && f->dgid==dgid && f->fn==(fn&7u) && f->ft==(ft&7u));
    if(dt==YSF_DT_VD2) {
        uint8_t dch[10];
        memcpy(dch,fn==1u?SRC_CALL "    ":fn==0u?DST_CALL "    ":"NOT-A-CALL",10);
        ysf_vd2_data_put(dch,frame);
        for(unsigned j=0;j<YSF_VCH_PER_FRAME;j++) {
            for(unsigned b=0;b<YSF_AMBE_BITS;b++)params[j][b]=(uint8_t)(rnd()&1u);
            ysf_vd2_voice_put(params[j],j,frame);
        }
    }
}
static void make_frame(uint8_t fi,uint8_t dt,uint8_t dgid,uint8_t fn,uint8_t frame[YSF_FRAME_BYTES],
                       uint8_t params[YSF_VCH_PER_FRAME][YSF_AMBE_BITS])
{ ysf_fich_t f;make_frame_ft(fi,dt,dgid,fn,7,frame,params,&f); }
/* A V/D mode 2 frame; when played, its five expected AMBE frames */
static void voice_frame(uint8_t dgid,uint8_t fn,bool played,uint8_t frame[YSF_FRAME_BYTES])
{
    uint8_t params[YSF_VCH_PER_FRAME][YSF_AMBE_BITS];
    make_frame(YSF_FI_COMMS,YSF_DT_VD2,dgid,fn,frame,params);
    if(played)for(unsigned j=0;j<YSF_VCH_PER_FRAME;j++) {
        uint8_t a72[AMBE_FRAME_BYTES];
        ambeFecEncode(params[j],a72);
        want_event(EV_FRAME,a72);
    }
}
static void add_voice(uint8_t dgid,uint8_t fn,bool played)
{ uint8_t frame[YSF_FRAME_BYTES];voice_frame(dgid,fn,played,frame);add_frame(frame); }
static void add_plain(uint8_t fi,uint8_t dt,uint8_t dgid)
{
    uint8_t frame[YSF_FRAME_BYTES],params[YSF_VCH_PER_FRAME][YSF_AMBE_BITS];
    make_frame(fi,dt,dgid,0,frame,params);add_frame(frame);
}

/* Voice FR (VW): IMBE parameters from the codec's encoder (a tone whose period and level change every frame, plus
 * noise), under the P25 IMBE FEC; the reference PCM is a fresh decoder's output for the same parameters */
static dm32_imbe_context enc_ctx,ref_ctx;
static int16_t want_pcm[MAX_PCM][160];
static unsigned nwant_pcm;
static void vw_call_begin(void) { dm32_imbe_init(&ref_ctx);nwant_pcm=0; }
static unsigned add_vw(uint8_t dgid,uint8_t fn,uint8_t ft,bool played,bool errors)
{
    uint8_t frame[YSF_FRAME_BYTES],params[YSF_VCH_PER_FRAME][YSF_AMBE_BITS];
    ysf_fich_t f;
    make_frame_ft(YSF_FI_COMMS,YSF_DT_VOICE_FR,dgid,fn,ft,frame,params,&f);
    unsigned n=ysf_vw_voice_count(&f);
    CHECK(n==((fn==0u && ft==1u)?2u:5u));		/* the FN 0 / FT 1 frame after the header: 2 sections */
    for(unsigned k=0;k<n;k++) {
        int16_t pcm[160];
        uint16_t u[8],back[8];
        uint8_t cw[YSF_VW_IMBE_BYTES],u0e=0;
        unsigned period=16u+rnd()%48u,level=2000u+rnd()%9000u;
        for(unsigned i=0;i<160u;i++) {
            unsigned phase=i%period;
            int tri=(int)(phase<period/2u?phase:period-phase)*(int)level*4/(int)period-(int)level;
            pcm[i]=(int16_t)(tri+(int)(rnd()%257u)-128);
        }
        CHECK(dm32_imbe_encode(&enc_ctx,pcm,u));
        p25_imbe_encode(u,cw);
        CHECK(p25_imbe_decode(cw,back,&u0e)==0 && !memcmp(back,u,sizeof u));
        if(errors) {
            /* Within the FEC: one bit in Golay c0, two in Golay c1, one in Hamming c4 (the worker plays frames with
             * at most 10 corrected bits and at most 3 in c0) */
            static const uint8_t at[]={5,24,40,96};
            for(unsigned e=0;e<sizeof at;e++)flipb(cw,at[e]+(k%3u));
            CHECK(p25_imbe_decode(cw,back,&u0e)==4 && u0e==1 && !memcmp(back,u,sizeof u));
        }
        CHECK(ysf_vw_voice_put(cw,&f,k,frame));
        if(played) {
            assert(nwant_pcm<MAX_PCM);
            CHECK(dm32_imbe_decode(&ref_ctx,u,want_pcm[nwant_pcm++]));
        }
    }
    add_frame(frame);
    return played?n:0;
}

/* Inverse of p25_phy_rx_air(): its air bit stream is RAM segments (from, count) in this order */
static const uint16_t layout01[][2]={{0,24},{72,108},{24,48},{180,108}};
static const uint16_t layout2[][2]={{0,24},{92,98},{24,10},{44,48},{34,10},{190,98}};
static void air_to_ram(const uint8_t air[36],uint8_t kind,uint8_t ram[36])
{
    const uint16_t (*seg)[2]=kind<2?layout01:layout2;
    unsigned n=kind<2?4u:6u,a=0;
    memset(ram,0,36);
    for(unsigned s=0;s<n;s++)for(unsigned i=0;i<seg[s][1];i++,a++)putb(ram,seg[s][0]+i,getb(air,a));
    assert(a==288);
}
/* Cut the stream into records (kinds[] cycles over the records); pad the last with zeros */
static void make_records(const uint8_t *kinds,unsigned nkinds)
{
    rec_count=(stream_bits+287u)/288u;assert(rec_count<=MAX_RECORDS);
    for(unsigned r=0;r<rec_count;r++) {
        uint8_t air[36]={0},back[36];
        for(unsigned i=0;i<288u;i++)if(r*288u+i<stream_bits)putb(air,i,getb(stream,r*288u+i));
        rec_kind[r]=kinds[r%nkinds];
        if(rec_kind[r]==LOST)continue;
        if(rec_kind[r]<3) {
            air_to_ram(air,rec_kind[r],rec_raw[r]);
            CHECK(!p25_phy_rx_air(rec_raw[r],rec_kind[r],back) && !memcmp(back,air,36));
        } else memcpy(rec_raw[r],air,36);	/* an unsupported layout: the worker must drop it */
    }
}

/* ---- Running the worker ---- */

static void execute_audio(uint8_t channel_dgid,bool vw)
{
    clock_ms=depth=turn=0;nev=voice_stops=voice_stop_at=0;
    receiving_ticks=frame_events=last_frame_ms=0;stream_end_ms=0;
    audio_starts=audio_stops=route_calls=nplayed=caller_after_end=0;callback=NULL;
    seen_src[0]=seen_dst[0]='\0';
    maximum_ms=rec_count*30u+3000u;
    CHECK(p25_lab_set_channel(0xF7E,0xF7E,0,0,0));
    CHECK(p25_lab_set_ysf(1,channel_dgid));
    CHECK(p25_lab_channel_rx(439000000));
    if(!setjmp(finished))worker_fn(NULL);
    CHECK(state==2 && starts && !owned && !depth);
    if(vw)CHECK(audio_starts==1 && audio_stops==1 && !callback);	/* VW plays on the P25 PCM path */
    else CHECK(!audio_starts && !audio_stops && !route_calls);	/* DN: the DMR vocoder owns audio */
    CHECK(!fifo.overflows);
    CHECK(voice_stops==1);			/* leaving YSF stops the vocoder stream once */
    CHECK(!voice.rx_frames && !unsupported);	/* nothing went to the P25 decoder */
    /* The worker ends 500 ms after the last good FICH (the normal channel owner then rearms) */
    CHECK(voice_stop_at>=stream_end_ms+400u && voice_stop_at<=stream_end_ms+700u);
    /* No callsign once the call ended, nor after the run */
    char src[11]="x",dst[11]="x";
    p25_lab_ysf_caller(src,dst);
    CHECK(!src[0] && !dst[0]);
    CHECK(!caller_after_end);
}
static void execute(uint8_t channel_dgid) { execute_audio(channel_dgid,false); }
/* The sink saw exactly want[] (begin / frames / end), the last end possibly from the worker's exit */
static void expect_events(const char *what)
{
    unsigned before=failures;
    CHECK(nev==nwant);
    for(unsigned i=0;i<nev && i<nwant;i++) {
        CHECK(ev[i].type==want[i].type);
        if(ev[i].type==EV_FRAME && want[i].type==EV_FRAME)CHECK(!memcmp(ev[i].a72,want[i].a72,AMBE_FRAME_BYTES));
    }
    if(nev && ev[nev-1].type==EV_FRAME)CHECK(!"a call was left open");
    printf("%s %s: %u sink events\n",failures==before?"ok  ":"FAIL",what,nev);
}
static unsigned count_events(uint8_t type) { unsigned n=0;for(unsigned i=0;i<nev;i++)n+=ev[i].type==type;return n; }

/* (1)+(2) a call split across 30 ms records; the terminator ends it before the worker does */
static void test_call(const uint8_t *kinds,unsigned nkinds,const char *what)
{
    reset_stream_builder();
    add_noise(173);				/* not record aligned */
    add_plain(YSF_FI_HEADER,YSF_DT_VD2,9);	/* the header is not voice */
    want_event(EV_BEGIN,NULL);
    for(unsigned k=0;k<7;k++)add_voice(9,(uint8_t)k,true);
    add_plain(YSF_FI_TERMINATOR,YSF_DT_VD2,9);
    want_event(EV_END,NULL);
    add_noise(50);
    make_records(kinds,nkinds);
    execute(0);
    expect_events(what);
    CHECK(count_events(EV_BEGIN)==1 && count_events(EV_FRAME)==35);
    /* The end came from the terminator (about when the stream ended), not from the worker's exit */
    CHECK(nev && ev[nev-1].type==EV_END && ev[nev-1].at<=stream_end_ms+40u && ev[nev-1].at+400u<=voice_stop_at);
    CHECK(receiving_ticks>0);
    /* (8) the callsigns from the DCH while the call played: FN 1 source, FN 0 destination (trailing spaces cut);
     * "" after the terminator is checked by execute() */
    CHECK(!strcmp(seen_src,SRC_CALL) && !strcmp(seen_dst,DST_CALL));
}
/* (2) without a terminator the worker's exit closes the call; a second call after a terminator begins again */
static void test_two_calls(void)
{
    reset_stream_builder();
    add_noise(31);
    want_event(EV_BEGIN,NULL);
    for(unsigned k=0;k<3;k++)add_voice(0,(uint8_t)k,true);
    add_plain(YSF_FI_TERMINATOR,YSF_DT_VD2,0);
    want_event(EV_END,NULL);
    add_noise(200);
    want_event(EV_BEGIN,NULL);
    for(unsigned k=0;k<4;k++)add_voice(0,(uint8_t)k,true);
    want_event(EV_END,NULL);			/* no terminator: the worker ends it */
    make_records((const uint8_t[]){0},1);
    execute(0);
    expect_events("two calls, second without terminator");
    CHECK(nev && ev[nev-1].type==EV_END && ev[nev-1].at==voice_stop_at);
}
/* (3) DG-ID: a channel DG-ID plays only its own; 0 plays all. V/D mode 1 and data frames are never voice */
static void test_dgid(void)
{
    static const uint8_t ids[]={7,5,0,5,127,5};
    for(unsigned pass=0;pass<2;pass++) {
        uint8_t channel=pass?0:5;
        reset_stream_builder();
        add_noise(77);
        bool open=false;
        for(unsigned k=0;k<sizeof ids;k++) {
            bool play=!channel || ids[k]==channel;
            if(play && !open) { want_event(EV_BEGIN,NULL);open=true; }
            add_voice(ids[k],(uint8_t)k,play);
            add_plain(YSF_FI_COMMS,YSF_DT_VD1,ids[k]);		/* V/D mode 1: not played (yet) */
        }
        add_plain(YSF_FI_COMMS,YSF_DT_DATA_FR,5);
        if(open)want_event(EV_END,NULL);
        make_records((const uint8_t[]){1},1);
        execute(channel);
        expect_events(pass?"DG-ID 0 accepts every DG-ID":"DG-ID 5 ignores 7, 0 and 127");
        CHECK(count_events(EV_FRAME)==(pass?30u:15u));
    }
}
/* (4) a frame whose FICH is broken is skipped; so is one whose sync is lost; the call goes on */
static void test_broken(void)
{
    reset_stream_builder();
    add_noise(5);
    want_event(EV_BEGIN,NULL);
    add_voice(3,0,true);
    add_voice(3,1,true);
    uint8_t frame[YSF_FRAME_BYTES];ysf_fich_t f;
    voice_frame(3,2,false,frame);
    for(unsigned i=0;i<40;i++)flipb(frame,40u+(i*37u)%200u);	/* 40 FICH bits: beyond Viterbi and Golay */
    CHECK(!ysf_fich_decode(frame,&f));
    add_frame(frame);
    add_voice(3,3,true);
    voice_frame(3,4,false,frame);
    for(unsigned i=0;i<40;i+=4)flipb(frame,i);			/* sync 10 bits off: the framer misses it */
    add_frame(frame);
    add_voice(3,5,true);
    memset(frame,0xFF,sizeof frame);frame[0]=0x00;		/* garbage of a frame's length */
    add_frame(frame);
    add_voice(3,6,true);
    add_plain(YSF_FI_TERMINATOR,YSF_DT_VD2,3);
    want_event(EV_END,NULL);
    make_records((const uint8_t[]){0},1);
    uint32_t bad=ysf_bad;
    execute(3);
    expect_events("broken FICH / lost sync skipped, call continues");
    CHECK(ysf_bad>bad);
    CHECK(count_events(EV_BEGIN)==1 && count_events(EV_FRAME)==25);
}
/* (5) one bit of every voice triplet wrong, a few FICH bits and sync bits wrong: same AMBE frames */
static void test_bit_errors(void)
{
    reset_stream_builder();
    add_noise(99);
    want_event(EV_BEGIN,NULL);
    for(unsigned k=0;k<5;k++) {
        uint8_t frame[YSF_FRAME_BYTES],p[YSF_AMBE_BITS];ysf_fich_t f;
        voice_frame(4,(uint8_t)k,true,frame);
        for(unsigned j=0;j<YSF_VCH_PER_FRAME;j++) {
            /* ysf.c: channel j starts at 240 + 40 + 144 j; its bit i sits at (i % 26) * 4 + i / 26 */
            for(unsigned t=0;t<27u;t++) {
                unsigned i=3u*t+(t+j+k)%3u;
                flipb(frame,240u+40u+144u*j+(i%26u)*4u+i/26u);
            }
            CHECK(ysf_vd2_voice(frame,j,p)==27u);		/* every triplet outvoted, none lost */
        }
        flipb(frame,1);flipb(frame,22);				/* two sync bits */
        flipb(frame,40u+k);flipb(frame,120u);flipb(frame,233u);	/* three FICH bits */
        CHECK(ysf_fich_decode(frame,&f) && f.dgid==4);
        add_frame(frame);
    }
    add_plain(YSF_FI_TERMINATOR,YSF_DT_VD2,4);
    want_event(EV_END,NULL);
    make_records((const uint8_t[]){2,0},2);
    execute(4);
    expect_events("bit errors corrected (triplets, FICH, sync)");
}
/* (6) a record the worker cannot use: an unsupported layout (kind 3, dropped by p25_phy_rx_air) or a record the
 * modem lost (a 60 ms time gap). The YSF frame in progress has a hole: it is dropped whole (no AMBE frame that was
 * not sent may play: a voice channel has no CRC), the framer hunts again and the next whole frame plays. */
static void disrupted(uint8_t bad_kind,const char *what)
{
    reset_stream_builder();
    add_noise(10);
    want_event(EV_BEGIN,NULL);
    for(unsigned k=0;k<8;k++)add_voice(0,(uint8_t)k,k!=3);	/* frame 3 (bits 2890..3849) has the hole */
    want_event(EV_END,NULL);
    static uint8_t kinds[MAX_RECORDS];
    memset(kinds,0,sizeof kinds);kinds[11]=bad_kind;		/* only record 11: bits 3168..3455 */
    make_records(kinds,MAX_RECORDS);
    uint32_t prior_gaps=gaps,prior_unsupported=ysf_unsupported;
    execute(0);
    if(bad_kind==LOST)CHECK(gaps==prior_gaps+1 && ysf_unsupported==prior_unsupported);
    else CHECK(ysf_unsupported==prior_unsupported+1);
    expect_events(what);
    CHECK(count_events(EV_BEGIN)==1 && count_events(EV_FRAME)==35);
}
static void test_disrupted(void)
{
    disrupted(3,"unsupported record layout (kind 3): broken frame dropped");
    disrupted(LOST,"lost record (60 ms gap): broken frame dropped");
}
/* (7) Voice FR (VW): played on the P25 PCM path, one 160-sample block per voice section, no erasures; the DMR sink
 * sees nothing. A frame of another DG-ID, V/D mode 1 and the header are not played */
static void vw_call(bool errors,const char *what)
{
    static const uint8_t fn_ft[][2]={{0,1},{1,1},{2,7},{3,7},{0,7},{5,7},{1,1},{6,7}};
    reset_stream_builder();
    vw_call_begin();
    add_noise(61);
    add_plain(YSF_FI_HEADER,YSF_DT_VOICE_FR,6);
    unsigned sections=0;
    for(unsigned k=0;k<sizeof fn_ft/sizeof fn_ft[0];k++) {
        sections+=add_vw(6,fn_ft[k][0],fn_ft[k][1],true,errors);
        if(k==3) {
            add_vw(9,1,7,false,false);				/* another DG-ID */
            add_plain(YSF_FI_COMMS,YSF_DT_VD1,6);
        }
    }
    add_plain(YSF_FI_TERMINATOR,YSF_DT_VOICE_FR,6);
    add_noise(40);
    make_records((const uint8_t[]){0,1,2},3);
    uint32_t prior_erasures=ysf_vw_erasures;
    execute_audio(6,true);
    unsigned before=failures;
    CHECK(sections==2u+5u*7u && nwant_pcm==sections);
    CHECK(!nev);						/* nothing for the DMR vocoder */
    CHECK(ysf_vw_erasures==prior_erasures);
    CHECK(pcm_blocks==sections && nplayed==sections);
    unsigned nonzero_blocks=0,equal=0;
    for(unsigned i=0;i<nplayed && i<nwant_pcm;i++) {
        equal+=!memcmp(played_pcm[i],want_pcm[i],sizeof want_pcm[i]);
        for(unsigned j=0;j<160u;j++)if(want_pcm[i][j]) { nonzero_blocks++;break; }
    }
    CHECK(equal==sections && nonzero_blocks>sections/2u);
    printf("%s %s: %u PCM blocks played (%u equal to the reference decode), erasures %u\n",
           failures==before?"ok  ":"FAIL",what,nplayed,equal,(unsigned)(ysf_vw_erasures-prior_erasures));
}
static void test_vw(void)
{
    vw_call(false,"VW call on the P25 PCM path");
    vw_call(true,"VW call with bit errors within the IMBE FEC");
}

#if CONFIG_CALL_LOG
/* DM32 Plus call history (p25_lab.c call_track): a call per 1.5 s gap or talkgroup/NAC change, the source filled in
 * late, encrypted when no clear superframe arrives in the first 0.5 s, clear once one does */
static void test_call_log(void)
{
    p25_lab_call_t c;
    memset(&call_now,0,sizeof call_now);
    CHECK(p25_lab_call(&c)==0);
    voice.active_group=101;voice.active_nac=0x659;voice.active_source=0;voice.crypto_clear=1;
    call_track(1000);                                   /* HDU: no source yet */
    CHECK(p25_lab_call(&c)==1 && c.tg==101 && c.nac==0x659 && c.src==0 && c.clear && !c.encrypted);
    voice.active_source=77285;
    call_track(1180);                                   /* LDU1 names it: the same call */
    CHECK(p25_lab_call(&c)==1 && c.src==77285 && c.start_ms==1000 && c.last_ms==1180);
    call_track(2600);                                   /* 1.42 s on: still the same call */
    CHECK(p25_lab_call(&c)==1);
    call_track(4200);                                   /* 1.6 s gap: a new call */
    CHECK(p25_lab_call(&c)==2 && c.start_ms==4200 && c.src==77285);
    voice.active_group=202;
    call_track(4300);                                   /* another talkgroup: a new call */
    CHECK(p25_lab_call(&c)==3 && c.tg==202);
    /* Encrypted: no clear superframe */
    voice.active_group=101;voice.active_source=1234;voice.crypto_clear=0;
    call_track(9000);
    CHECK(p25_lab_call(&c)==4 && !c.encrypted);         /* too early to tell */
    call_track(9360);
    CHECK(p25_lab_call(&c)==4 && !c.encrypted);         /* 0.36 s */
    call_track(9540);
    CHECK(p25_lab_call(&c)==4 && c.encrypted && !c.clear);
    /* A late clear superframe (late entry) makes it clear */
    voice.crypto_clear=1;
    call_track(9720);
    CHECK(p25_lab_call(&c)==4 && c.clear && !c.encrypted);
    /* Another NAC: a new call */
    voice.active_nac=0x293;
    call_track(9900);
    CHECK(p25_lab_call(&c)==5 && c.nac==0x293);
    memset(&call_now,0,sizeof call_now);
    memset(&voice,0,sizeof voice);
    puts("call log: calls by gap / talkgroup / NAC, late source, encrypted after 0.5 s without clear voice");
}
#endif

int main(void)
{
#if CONFIG_CALL_LOG
    test_call_log();
#endif
    p25_lab_start();beep_init(&audio);
    dm32_imbe_init(&enc_ctx);
    test_call((const uint8_t[]){0},1,"call, kind 0 records, terminator ends it");
    test_call((const uint8_t[]){1},1,"call, kind 1 records");
    test_call((const uint8_t[]){2},1,"call, kind 2 records");
    test_call((const uint8_t[]){0,2,1,2},4,"call, mixed record kinds");
    test_two_calls();
    test_dgid();
    test_broken();
    test_bit_errors();
    test_disrupted();
    test_vw();
    printf("ysf worker: %u failures (frames %u, bad FICH %u, other %u, voice %u, unsupported %u, VW erasures %u)\n",
           failures,(unsigned)ysf_frames,(unsigned)ysf_bad,(unsigned)ysf_other,(unsigned)ysf_voice,
           (unsigned)ysf_unsupported,(unsigned)ysf_vw_erasures);
    return failures?1:0;
}
