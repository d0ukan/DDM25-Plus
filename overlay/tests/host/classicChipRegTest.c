// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Open25-DM32 Contributors
// The actual product handler on the existing register-hook fixtures.
#define main upstream_register_fixture_main
#include "debuglinkChipRegTest.c"
#undef main

int main(void)
{
	const uint8_t writes[][9] = {
		{0,0,0x30,0xfe,0xc1,0,0},	// RF TX state
		{0,0,0x38,0xff,0xff,0,0},	// synthesizer frequency
		{1,0,0x03,0x7f,0,0,0},	// write-only chip
		{DL_CHIP_SOC,0x08,0,0,0x17,0x40,0,0,0},	// SoC GPIO/timer path
	};
	for(unsigned on_air=0;on_air<2;on_air++){
		reset();fake_tx=on_air;
		for(unsigned i=0;i<sizeof writes/sizeof writes[0];i++){
			CHECK(cmd(writes[i],i==3?9:7)==1&&out[0]==DL_ST_REFUSED);
			CHECK(!logs&&!soc_calls&&!soc_word&&!chip0[0][0x30]&&!chip0[0][0x38]);
		}
		const uint8_t rf_read[]={0,1,0x12},soc_read[]={DL_CHIP_SOC,0x08,0,0,0x17};
		chip0[1][0x12]=0xbeef;
		CHECK(cmd(rf_read,sizeof rf_read)==5&&out[0]==DL_ST_OK&&reply32()==0xbeef);
		CHECK(cmd(soc_read,sizeof soc_read)==5&&out[0]==DL_ST_OK&&reply32()==0x80000000u);
		CHECK(!logs&&soc_calls==1);
	}
	printf("Classic CHIP_REG: %d checks, %s; RF/SoC writes refused before hooks, reads retained\n",checks,failures?"FAILED":"PASS");
	return failures!=0;
}
