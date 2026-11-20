/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus separate RX and TX NAC (Classic r13's split NAC, CONFIG_P25_TXNAC extras; docs/plus.md "Separate RX
 * NAC and TX NAC") in the real radio core and native controller, after the Classic suite, the multi-watch ring and
 * the VFO's own suite (tests/classic/test_radio.inc, tests/plus/test_mwatch_radio.inc, test_vfox_radio.inc) run again
 * with it built in. The worker and RF are the suite's fixtures: nothing transmits. Checked: the worker is armed with
 * the RX NAC and the effective TX NAC (Classic r13's p25_lab_set_channel); the TX gate takes the effective TX NAC
 * (RX Any inherits $293, a TX NAC of its own is used, an explicit Any is refused); a saved TX NAC edit restarts the
 * receiver so the next PTT sends with it; and, built with CONFIG_VFO_MODES, the VFO's rules (vfox_fix, Copy to VFO,
 * the C..F slots keeping the TX NAC). */
#include <stdio.h>
#if CONFIG_VFO_MODES
#include "radio/vfox.h"
#endif

/* A P25 channel with RX NAC rx, TX NAC tx (0xFFFF: Same as RX), its receiver started */
static void nac_select(uint16_t rx,uint16_t tx)
{
    setup_codeplug();start();native_lab_init();
    fake_power.switch_on=classic_on;fake_power.battery_mv=classic_battery;
    classic_busy=classic_stop=classic_receiving=false;classic_starts=classic_restores=0;
    classic_set_tx_nac=0xFFFF;
    sm.view.ch.mode=RADIO_MODE_P25;sm.view.ch.bw=RADIO_BW_12K5;
    sm.view.ch.p25_nac=rx;sm.view.ch.p25_tg=42;sm.view.ch.p25_id=3101234;sm.view.ch.p25_squelch=1;
    sm.view.ch.p25_tx_nac_override=tx==0xFFFFu?0:(uint16_t)(tx+1u);
    sm.view.power=2;
    classic_pump();classic_pump();
}
/* PTT: armed for TX on the channel's TX frequency, then released */
static void nac_talks(void)
{
    radio_req_t p={.type=RREQ_PTT,.arg=1};
    CHECK(native_lab_request(&sm,&p)==1 && sm.view.tx_deny==TXDENY_NONE);
    classic_pump();classic_pump();
    CHECK(classic_live->config.mode==NT_TRANSMIT && classic_live->state==NT_ARMED);
    p.arg=0;CHECK(native_lab_request(&sm,&p)==1);
    classic_pump();classic_pump();classic_pump();
    CHECK(classic_live->state==NT_RX && !sm.ptt && !hw.tx_calls);
}

static void test_plus_txnac(void)
{
    int before=failures;
    /* (a) a repeater: the receiver filters on $293, the worker sends $659 */
    nac_select(0x293,0x659);
    CHECK(classic_live && classic_live->state==NT_RX && native_lab_owns());
    CHECK(classic_set_nac==0x293 && classic_set_tg==42 && classic_set_tx_nac==0x659);
    CHECK(codeplug_p25_valid(&sm.view.ch) && radio_sm_tx_check(&sm)==TXDENY_NONE);
    nac_talks();
    printf("%s RX $293 / TX $659: receiver on $293, the worker's TX NAC $659, PTT armed\n",failures==before?"ok  ":"FAIL");

    /* Same as RX: the worker's TX NAC is the RX NAC */
    before=failures;
    nac_select(0x293,0xFFFF);
    CHECK(classic_set_nac==0x293 && classic_set_tx_nac==0x293 && radio_sm_tx_check(&sm)==TXDENY_NONE);
    nac_talks();
    printf("%s Same as RX: TX with $293\n",failures==before?"ok  ":"FAIL");

    /* (b) RX Any + TX $659: hears every NAC, talks with $659 */
    before=failures;
    nac_select(0xF7E,0x659);
    CHECK(classic_live && classic_live->state==NT_RX && classic_set_nac==0xF7E && classic_set_tg==42);
    CHECK(classic_set_tx_nac==0x659 && radio_sm_tx_check(&sm)==TXDENY_NONE);
    nac_talks();
    /* RX Any, Same as RX: Classic r13 sends $293 */
    nac_select(0xF7E,0xFFFF);
    CHECK(classic_set_nac==0xF7E && classic_set_tx_nac==0x293 && radio_sm_tx_check(&sm)==TXDENY_NONE);
    /* An explicit TX NAC Any: refused (not a valid channel) */
    sm.view.ch.p25_tx_nac_override=0xF7E + 1;
    CHECK(!codeplug_p25_valid(&sm.view.ch) && radio_sm_tx_check(&sm)==TXDENY_MODE);
    /* TX NAC set, but no talkgroup / no ID: refused as before */
    sm.view.ch.p25_tx_nac_override=0x659+1;
    sm.view.ch.p25_tg=0;
    CHECK(radio_sm_tx_check(&sm)==TXDENY_MODE);
    sm.view.ch.p25_tg=42;sm.view.ch.p25_id=0;
    CHECK(radio_sm_tx_check(&sm)==TXDENY_MODE);
    printf("%s RX Any + TX $659: armed; Same as RX sends $293; explicit Any, no talkgroup, no ID refused\n",
           failures==before?"ok  ":"FAIL");

#if CONFIG_P25_ANY
    /* A saved TX NAC edit (codeplug_serial, no request) restarts the receiver: the worker gets the new TX NAC */
    before=failures;
    nac_select(0x293,0xFFFF);
    CHECK(classic_set_tx_nac==0x293);
    unsigned sets=classic_sets;uint32_t old=classic_sid;
    sm.view.ch.p25_tx_nac_override=0x125+1;
    for(unsigned i=0;i<4;i++)classic_pump();
    CHECK(classic_sets==sets+1 && classic_set_nac==0x293 && classic_set_tx_nac==0x125);
    CHECK(classic_sid!=old && classic_live->state==NT_RX && native_lab_owns());
    nac_talks();
    /* and back to Same as RX */
    sm.view.ch.p25_tx_nac_override=0;
    for(unsigned i=0;i<4;i++)classic_pump();
    CHECK(classic_sets==sets+2 && classic_set_tx_nac==0x293);
    printf("%s TX NAC edited on the channel in use: the worker takes it before the next PTT\n",
           failures==before?"ok  ":"FAIL");
#endif

#if CONFIG_VFO_MODES
    /* The VFO: its TX rule, Copy to VFO, the ring letters' compact slots (C..F, and A / B parked) */
    before=failures;
    channel_t v;
    memset(&v,0,sizeof v);
    v.rx_hz=v.tx_hz=433500000u;v.power=CODEPLUG_POWER_DEFAULT;v.color_code=1;v.timeslot=1;
    vfox_set_mode(&v,RADIO_MODE_P25,3101234);
    CHECK(v.p25_nac==0xF7E && !v.p25_tx_nac_override && v.rx_only && v.p25_squelch==1);	/* Any, Any: listens */
    v.p25_tg=99;vfox_fix(&v);
    CHECK(!v.rx_only && codeplug_p25_tx_nac(&v)==0x293);	/* Any NAC, Same as RX: Classic r13 sends $293 */
    v.p25_tx_nac_override=0x659+1;vfox_fix(&v);
    CHECK(!v.rx_only && codeplug_p25_tx_nac(&v)==0x659 && codeplug_p25_valid(&v));
    v.p25_tx_nac_override=0xF7E + 1;vfox_fix(&v);		/* never Any: dropped (Same as RX again) */
    CHECK(!v.p25_tx_nac_override);
    v.p25_tx_nac_override=0x2659;vfox_fix(&v);			/* not a TX NAC value */
    CHECK(!v.p25_tx_nac_override);
    /* the slot keeps it (13 bits) with the rest as they were */
    static const uint16_t slots[]={0,0x000+1,0x659+1,0xFFF+1};
    for(unsigned i=0;i<4;i++) {
        channel_t c=v,back;vfox_slot_t s;uint32_t talk;bool seeded;
        c.p25_nac=0x293;c.p25_tx_nac_override=slots[i];vfox_fix(&c);
        vfox_slot_put(&s,&c,0x80001234u,true);
        vfox_slot_get(&s,&back,&talk,&seeded);
        CHECK(seeded && talk==0x80001234u && back.p25_tx_nac_override==slots[i] && back.p25_nac==0x293 &&
              back.p25_tg==99 && back.p25_id==3101234 && back.bw==RADIO_BW_12K5 && back.mode==RADIO_MODE_P25 &&
              back.rx_only==c.rx_only);
    }
    /* an FM / DMR VFO's slot as before (no TX NAC), its 25 kHz kept */
    channel_t fm=v,back;vfox_slot_t s;uint32_t talk;bool seeded;
    vfox_set_mode(&fm,RADIO_MODE_FM,0);fm.bw=RADIO_BW_25K;vfox_fix(&fm);
    CHECK(!fm.p25_tx_nac_override);
    vfox_slot_put(&s,&fm,0,true);vfox_slot_get(&s,&back,&talk,&seeded);
    CHECK(back.mode==RADIO_MODE_FM && back.bw==RADIO_BW_25K && !back.p25_tx_nac_override);
    /* Copy to VFO of a channel with a TX NAC; to YSF, the TX NAC goes */
    channel_t ch=v;ch.p25_nac=0xF7E;ch.p25_tx_nac_override=0x125+1;ch.rx_only=false;
    channel_t vfo=fm;vfo.mode=RADIO_MODE_P25;
    vfox_copy(&vfo,&ch);
    CHECK(vfo.p25_tx_nac_override==0x125+1 && !vfo.rx_only);
    vfox_set_mode(&vfo,RADIO_MODE_YSF,0);
    CHECK(!vfo.p25_tx_nac_override);
    printf("%s VFO TX NAC: TX rule, never Any, the ring's slots, Copy to VFO\n",failures==before?"ok  ":"FAIL");
#endif
}

int main(void)
{
    int prior=vfox_radio_main();
    test_plus_txnac();
    printf("RX / TX NAC in the radio core: %d failures\n",failures);
    return prior || failures ? 1 : 0;
}
