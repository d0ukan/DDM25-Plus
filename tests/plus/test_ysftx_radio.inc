/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus YSF transmit gates in the real radio core and native controller (docs/ysf-nxdn-tx.md), after the whole
 * Classic suite (tests/classic/test_radio.inc). Built twice by tools/test_classic.py: CONFIG_YSF=1 with
 * CONFIG_YSF_TX=0 (a YSF channel's PTT is always refused, nothing is armed) and with CONFIG_YSF_TX=1 (refused without
 * a callsign, outside the P25 TX bands, RX only, TX inhibit; otherwise armed with our callsign handed to the worker,
 * and the channel time-out ends it). The worker and RF are the suite's fixtures: nothing transmits. */
#include <stdio.h>
static bool ysf_on;
static uint8_t ysf_dgid_set;
int p25_lab_set_ysf(int on,uint8_t dgid) { if(!classic_busy){ysf_on=on!=0;ysf_dgid_set=dgid;}return !classic_busy; }
uint32_t p25_lab_ysf_caller(char src[11],char dst[11]) { if(src)src[0]='\0';if(dst)dst[0]='\0';return 0; }
#if CONFIG_YSF_TX
static char ysf_call_set[11];
static unsigned ysf_call_sets;
int p25_lab_set_ysf_tx(const char *call) { snprintf(ysf_call_set,sizeof ysf_call_set,"%s",call);ysf_call_sets++;return 1; }
#endif

/* A YSF channel (DG-ID 0, TX allowed by its record), its receiver running */
static void ysf_select(bool rx_only)
{
    setup_codeplug();start();native_lab_init();
    fake_power.switch_on=classic_on;fake_power.battery_mv=classic_battery;
    classic_busy=classic_stop=classic_receiving=false;classic_starts=classic_restores=0;ysf_on=false;
    sm.view.ch.mode=RADIO_MODE_YSF;sm.view.ch.bw=RADIO_BW_12K5;
    sm.view.ch.p25_nac=0xF7E;sm.view.ch.p25_tg=0;sm.view.ch.p25_id=0;sm.view.ch.rx_only=rx_only;
    sm.view.power=2;
    snprintf(sm.radio_name,sizeof sm.radio_name,"%s","TA1ABC-7");
    classic_pump();classic_pump();
}
/* PTT down and up again: refused with reason why, nothing armed, the receiver as it was (when it ran) */
static void ysf_refused(uint8_t why)
{
    unsigned starts=classic_starts;
    radio_req_t p={.type=RREQ_PTT,.arg=1};
    CHECK(native_lab_request(&sm,&p)==1 && sm.view.state==RS_TX_DENIED && sm.view.tx_deny==why);
    classic_pump();classic_pump();
    CHECK(classic_starts==starts && !hw.tx_calls);
    CHECK(!classic_live || classic_live->config.mode!=NT_TRANSMIT || classic_live->state!=NT_ARMED);
    p.arg=0;CHECK(native_lab_request(&sm,&p)==1 && !sm.ptt);
    classic_pump();classic_pump();
}

static void test_plus_ysf_tx(void)
{
    int before=failures;
#if !CONFIG_YSF_TX
    /* No transmitter in this build: an RX only YSF channel says so; a record that allows TX isn't even valid here */
    ysf_select(true);
    CHECK(ysf_on && classic_live && classic_live->state==NT_RX && native_lab_owns());
    CHECK(radio_sm_tx_check(&sm)==TXDENY_RX_ONLY);
    ysf_refused(TXDENY_RX_ONLY);
    sm.view.ch.rx_only=false;
    CHECK(!codeplug_p25_valid(&sm.view.ch) && radio_sm_tx_check(&sm)==TXDENY_MODE);
    ysf_refused(TXDENY_MODE);
    printf("%s YSF transmit off: PTT on a YSF channel never arms the modem\n",failures==before?"ok  ":"FAIL");
#else
    ysf_select(false);
    CHECK(ysf_on && ysf_dgid_set==0 && classic_live && classic_live->state==NT_RX && native_lab_owns());
    CHECK(codeplug_p25_valid(&sm.view.ch) && radio_sm_tx_check(&sm)==TXDENY_NONE);
    channel_t saved=sm.view.ch;
    /* The gates, one at a time */
    snprintf(sm.radio_name,sizeof sm.radio_name,"%s","Ahmet");
    ysf_refused(TXDENY_NO_CALL);			/* the radio name is not a callsign */
    sm.radio_name[0]='\0';
    ysf_refused(TXDENY_NO_CALL);
    snprintf(sm.radio_name,sizeof sm.radio_name,"%s","TA1ABC-7");
    sm.view.ch.tx_hz=146500000u+2000000u;		/* 148.5 MHz: outside the P25 / YSF TX bands */
    ysf_refused(TXDENY_BAND);
    sm.view.ch=saved;sm.view.ch.rx_only=true;
    ysf_refused(TXDENY_RX_ONLY);
    sm.view.ch=saved;sm.set.tx_inhibit=true;
    ysf_refused(TXDENY_INHIBIT);
    sm.set.tx_inhibit=false;sm.batt.level=BATT_CRITICAL;
    ysf_refused(TXDENY_BATTERY);
    sm.batt.level=BATT_OK;
    CHECK(!ysf_call_sets);				/* no callsign went to the worker for a refused PTT */
    /* Allowed: our callsign to the worker, the modem armed for TX on the channel's TX frequency */
    radio_req_t p={.type=RREQ_PTT,.arg=1};
    CHECK(native_lab_request(&sm,&p)==1 && sm.view.tx_deny==TXDENY_NONE);
    CHECK(ysf_call_sets==1 && !strcmp(ysf_call_set,"TA1ABC"));
    classic_pump();classic_pump();
    CHECK(classic_live->config.mode==NT_TRANSMIT && classic_live->state==NT_ARMED);
    CHECK(classic_live->config.frequency_hz==sm.view.ch.tx_hz && classic_live->config.power==2);
    uint8_t raw[36]={0};
    CHECK(native_lab_live_block(classic_sid,raw,0) && native_lab_live_block(classic_sid,raw,0));
    CHECK(native_lab_live_go(classic_sid));classic_pump();
    CHECK(classic_live->state==NT_TX && sm.view.state==RS_TX && !hw.tx_calls);
    /* The channel time-out stops it as P25's */
    sm.view.tot_s=1;now+=1000;classic_pump();
    CHECK(!classic_live->owned && !classic_live->keyed && sm.view.tx_deny==TXDENY_TIMEOUT);
    p.arg=0;CHECK(native_lab_request(&sm,&p)==1);
    classic_pump();classic_pump();
    CHECK(classic_live->state==NT_RX && !sm.ptt && ysf_on && !hw.tx_calls);
    printf("%s YSF transmit gates: callsign, band, RX only, inhibit, battery; armed with TA1ABC; time-out\n",
           failures==before?"ok  ":"FAIL");
#endif
}

int main(void)
{
    int prior=classic_radio_main();
    test_plus_ysf_tx();
    printf("YSF transmit in the radio core (CONFIG_YSF_TX=%d): %d failures\n",CONFIG_YSF_TX,failures);
    return prior || failures ? 1 : 0;
}
