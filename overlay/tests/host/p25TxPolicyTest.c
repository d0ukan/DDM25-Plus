// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Open25-DM32 Authors
#include <assert.h>
#include <stdio.h>
#include "radio/p25_tx_policy.h"
static bool extended;
static const band_t bands[]={
 {136000000,174000000,true},{400000000,480000000,true},
 {764000000,776000000,false},{794000000,824000000,false},{851000000,870000000,false},
 {16000000,65000000,false},{174000000,400000000,false},{480000000,740000000,false}};
const board_t board={.bands=bands,.band_count=8,.rx_ext_count=3,.rx_ext_on=&extended};
int main(void)
{
 assert(!extended);
#if CONFIG_RX_EXT_ALWAYS
 assert(board_in_band(200000000,false));
 board_rx_ext_set(false);assert(board_in_band(200000000,false));
#else
 assert(!board_in_band(200000000,false));board_rx_ext_set(true);assert(board_in_band(200000000,false));
#endif
 const uint32_t allowed[]={144000000,147999999,420000000,449999999,
 151820000,151900001,151940000,154570000,154585001,154600000,
 462550000,462600001,462725000,467550000,467600001,467725000};
 for(unsigned i=0;i<sizeof allowed/sizeof *allowed;i++)assert(radio_p25_tx_allowed(allowed[i]));
#if CONFIG_P25_TX_BAND_POLICY
 const uint32_t denied[]={143999999,148000000,419999999,450000000,
 151819999,151940001,152000000,154569999,154600001,
 462549999,462725001,467549999,467725001};
 for(unsigned i=0;i<sizeof denied/sizeof *denied;i++)assert(!radio_p25_tx_allowed(denied[i]));
#endif
 unsigned checked=0;
 for(uint32_t hz=136000000;hz<=480000000;hz+=1250) {
  bool hardware=(hz>=136000000&&hz<=174000000)||(hz>=400000000&&hz<=480000000);
  assert(board_in_band(hz,true)==hardware); // Ordinary FM/DMR/NXDN coverage unchanged.
  bool expected=hardware;
#if CONFIG_P25_TX_BAND_POLICY
  expected=(hz>=144000000&&hz<148000000)||(hz>=420000000&&hz<450000000);
  expected=expected||(hz>=151820000&&hz<=151940000)||(hz>=154570000&&hz<=154600000)||
   (hz>=462550000&&hz<=462725000)||(hz>=467550000&&hz<=467725000);
#endif
  assert(radio_p25_tx_allowed(hz)==expected);checked++;
 }
 const uint32_t edges[]={16000000,65000000,200000000,740000000,764000000,776000000,794000000,824000000,851000000,870000000};
 for(unsigned i=0;i<sizeof edges/sizeof *edges;i++){assert(board_in_band(edges[i],false));assert(!board_in_band(edges[i],true));assert(!radio_p25_tx_allowed(edges[i]));}
 assert(!board_in_band(100000000,false));assert(!board_in_band(750000000,false));
 printf("P25 TX policy and receive coverage: %u grid frequencies,continuous service ranges,interior/edge/settings checks passed\n",checked);
 return 0;
}
