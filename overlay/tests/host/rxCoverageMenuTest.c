// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Open25-DM32 Authors
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_screens.h"
// Settings-list enumeration is also used by the UI search owner.
uint8_t ui_set_group_count(void);
uint8_t ui_set_item_count(uint8_t group);
const char *ui_set_item_name(uint8_t group,uint8_t item,bool english,const char **synonyms);
int main(void)
{
 sim_reset(sim_geom_find("240x320"),SIM_MAP_K);
 unsigned items=0;
 for(unsigned g=0;g<ui_set_group_count();g++)for(unsigned i=0;i<ui_set_item_count(g);i++) {
  const char *synonyms;
  const char *name=ui_set_item_name(g,i,true,&synonyms);
  assert(name && !strstr(name,"RX past bands"));items++;
 }
 assert(items>20);
 printf("Always-on RX menu: %u real setting items inspected; runtime option absent\n",items);
 return 0;
}
