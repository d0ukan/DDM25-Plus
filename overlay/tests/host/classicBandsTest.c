// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Open25-DM32 Contributors
// The runner extracts the current production descriptor and RX/TX/editor functions.
// Hardware doubles count side effects; no radio, PA, calibration or serial port is used.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "codeplug/codeplug_tag.h"
#include "nxdn/nxdn_codeplug.h"
#include "radio/radio_bands.h"
#include "rf/fd6818/fd6818.h"
#define TR(key, text) (text)
#include "classic_band_table.inc"

static unsigned hardware, pa_keys, burst_arms, calibration;
static uint32_t tuned;
static const fd6818_variant_t variant = {.bw_fm_25k=0x3028,.bw_fm_12k5=0x4008,.bw_dmr=0x4048};
static fd6818_t fd = {.variant=&variant};
static int modem;
static radio_mode_t mode;
static bool rf_b, rf_unlocked, have_rx, asleep, transmitting, tx_uhf, tx_wide, dmr_keyed;
static uint32_t rx_hz, tx_hz;
static uint16_t tx_drive, drive_override;
static radio_bw_t rx_bw;
static subtone_t rx_tone;
static channel_t ced_ch;
enum { PIN_MIC_PATH, C7_AUDIO_FM_PLAY, BF_ROW_REG40 };
static void wake(void){hardware++;}
static void dmr_disarm(void){hardware++;dmr_keyed=false;}
static void pa_off(void){hardware++;}
static void pa_key(void){hardware++;pa_keys++;}
static void lna(uint32_t hz,bool on){hardware++;}
static void speaker_source(void){hardware++;}
static void ring_play(void){hardware++;}
static uint32_t synth_hz(uint32_t hz){return hz;}
static uint16_t level_drive(uint32_t hz,uint8_t level){calibration++;return 100;}
static void keyup_plan(uint32_t hz){calibration++;}
static void write_mod_bias(uint32_t hz){calibration++;}
static bool bf_cal_valid(void){return false;}
static uint8_t bf_cal_tx_byte(uint32_t hz,unsigned row){calibration++;return 0;}
static fd6818_tone_t fd_tone(uint8_t tone){return (fd6818_tone_t)tone;}
static unsigned c7_tone(uint8_t tone){return tone;}
static void hrc_dac_b_power(bool on){hardware++;}
static void hrc_pin_write(unsigned pin,bool on){hardware++;}
static void hrc_rfburst_arm(void){hardware++;burst_arms++;}
static void hrc7000_fm_rx(int *m,bool wide){hardware++;}
static void hrc7000_fm_tx(int *m,bool wide){hardware++;}
static void hrc7000_audio_bits(int *m,unsigned clear,unsigned set){hardware++;}
static void hrc7000_set_subtone(int *m,bool tx,unsigned tone,uint16_t value){hardware++;}
static void hal_delay_ms(uint32_t ms){hardware++;}
void fd6818_write(fd6818_t *dev,uint8_t reg,uint16_t value){hardware++;}
void fd6818_set_freq(fd6818_t *dev,uint32_t hz){hardware++;tuned=hz;}
void fd6818_set_bandwidth(fd6818_t *dev,uint16_t bw){hardware++;dev->bw_reg=bw;}
void fd6818_set_state(fd6818_t *dev,uint16_t state){hardware++;dev->state=state;}
void fd6818_set_rx_tone(fd6818_t *dev,fd6818_tone_t tone,uint16_t value){hardware++;}
void fd6818_fm_tx_prepare(fd6818_t *dev){hardware++;}
#include "classic_band_functions.inc"

static void refused(uint32_t hz)
{
	hardware=pa_keys=burst_arms=calibration=0;
	transmitting=dmr_keyed=false;tx_hz=146520000;
	assert(!tx(hz,RADIO_BW_12K5,4,NULL));
	assert(!hardware&&!pa_keys&&!burst_arms&&!calibration);
	assert(!transmitting&&!dmr_keyed&&tx_hz==146520000);
}

int main(void)
{
	const uint32_t ranges[][2]={{764000000,776000000},{794000000,806000000},
		{806000000,824000000},{851000000,870000000}};
	const uint32_t gaps[]={740000001,763999999,776000001,793999999,824000001,850999999,870000001};
	for(unsigned enabled=0;enabled<2;enabled++){
		board_rx_ext_set(enabled);
		for(unsigned i=0;i<sizeof gaps/sizeof gaps[0];i++){
			assert(!board_in_band(gaps[i],false));refused(gaps[i]);
		}
		for(unsigned r=0;r<4;r++)for(unsigned edge=0;edge<2;edge++){
			uint32_t hz=ranges[r][edge];
			assert(board_in_band(hz,false)&&!board_in_band(hz,true)&&board_tx_band(hz)<0);
			assert(!board_offset_carries(446050000,hz,451050000));
			for(unsigned logical=RADIO_MODE_FM;logical<=RADIO_MODE_NXDN;logical++){
				// P25/NXDN use the same board digital mode, per hal_radio.h.
				mode=logical==RADIO_MODE_FM?RADIO_MODE_FM:RADIO_MODE_DMR;
				assert(rx(hz,RADIO_BW_12K5,NULL)&&tuned==hz&&fd.state==FD6818_STATE_RX&&!transmitting);
				channel_t ch={.name="RX",.mode=logical,.rx_hz=hz,.tx_hz=451050000};
				ced_rx_band(&ch);assert(ch.mode==logical&&ch.rx_only&&ch.tx_hz==hz);
				ced_ch=ch;assert(ced_problem()==NULL);
				ced_ch.rx_only=false;assert(ced_problem()!=NULL);
				// The board refusal has no stored RX-only bit or RX/offset dependency.
				rx_hz=146520000;refused(hz);rx_hz=hz;refused(hz);
			}
		}
	}
	for(unsigned m=RADIO_MODE_FM;m<=RADIO_MODE_DMR;m++)for(unsigned i=0;i<4;i++){
		const uint32_t ordinary[]={136000000,174000000,400000000,480000000};
		mode=(radio_mode_t)m;hardware=pa_keys=burst_arms=calibration=0;rf_unlocked=false;
		assert(rx(ordinary[i],RADIO_BW_12K5,NULL));
		assert(tx(ordinary[i],RADIO_BW_12K5,1,NULL)&&transmitting&&tx_hz==ordinary[i]);
		assert(calibration&&((m==RADIO_MODE_FM&&pa_keys==1&&!burst_arms)||
			(m==RADIO_MODE_DMR&&!pa_keys&&burst_arms==1)));
	}
	puts("PASS Classic RX bands: exact inclusive edges/gaps, RX_EXT off/on, FM/DMR/P25/NXDN receive, editor mode retention and actual board TX/PA/burst-arm refusal");
}
