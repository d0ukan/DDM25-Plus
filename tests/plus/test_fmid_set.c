/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Settings > Talk & listen > FM caller ID (UI_SET_FM_ID) through the actual UI storage service and settings
 * store on the DM32 flash map (sim_flash): on in a new radio, off and on again kept across power cycles, stored as
 * settings_t.cp_flags bit 7 (SETTINGS_CP_NO_FM_ID) next to the codeplug acceptance bit, which neither disturbs; a
 * factory reset's defaults read on. Built by tools/test_classic.py like
 * tests/classic/test_start_new.c, with -DCONFIG_FM_ID=1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ui_svc.h"
#include "storage/storage.h"
#include "settings/settings.h"
#include "radio/radio.h"
#include "sim/sim_flash.h"
#include "dmr/dmr_data_api.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)
#define SIZE 0x1000000u
static const hal_flash_map_t map = {
    .settings_addr=0x711000, .settings_size=0x8000, .journal_addr=0x719000,
    .openfw_eeprom=0x700000, .openfw_flash=0x720000, .openfw_end=0xF00000,
    .native_addr=0xF00000, .native_size=0x100000
};
static const band_t bands[] = {{136000000,174000000,true},{400000000,480000000,true}};
const board_t board = {.name="DM32 test", .flash=&sim_flash, .bands=bands, .band_count=2, .power_count=9};
void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}
uint32_t hal_millis(void) { return 10000; }
void radio_get_view(radio_view_t *out) { memset(out,0,sizeof(*out)); }
void radio_req_clear_temporary(void) {}
void radio_req_dmr_target_clear(void) {}
bool ui_notify(const ui_event_t *ev) { (void)ev; return true; }
/* The rest of ui_svc_setting_get / _set's world (other settings), never reached here */
void radio_get_settings(radio_settings_t *s) { memset(s,0,sizeof(*s)); }
uint8_t radio_volume_step(uint8_t stored) { return stored; }
uint8_t radio_volume_stored(uint8_t step) { return step; }
void radio_req_volume(uint8_t step) { (void)step; }
void radio_req_reload(void) {}
void radio_req_dmr_privacy(uint8_t type, uint16_t key) { (void)type; (void)key; }
int32_t ui_svc_ram_get(uint8_t id) { (void)id; return 0; }
bool ui_svc_ram_set(uint8_t id, int32_t value) { (void)id; (void)value; return true; }
int32_t ui_svc_reply_shown(const radio_settings_t *s) { (void)s; return 0; }
#if CONFIG_DMR_DATA_CORE
void dmr_data_get_settings(dmrDataSettings_t *out) { memset(out,0,sizeof(*out)); }
void dmr_data_set_settings(const dmrDataSettings_t *in) { (void)in; }
#endif
void zone_share_set_offers(uint8_t mode) { (void)mode; }
void hostlink_settings_changed(void) {}
void hostlink_set_hotspot_allowed(bool allowed) { (void)allowed; }
void tftp_codeplug_allow_write(bool allow) { (void)allow; }

static uint8_t cp_flags(void)
{
    settings_t st;
    settings_read(&st);
    return st.cp_flags;
}

static void power_cycle(void)
{
    CHECK(settings_flush());
    sim_flash_power_on();
    CHECK(storage_init());
    CHECK(settings_init()==SETTINGS_LOADED);
}

int main(void)
{
    sim_flash_create(SIZE,&map);
    memset(sim_flash_data,0xFF,SIZE);
    CHECK(storage_init());
    settings_init();
    /* A new radio: on, nothing stored */
    CHECK(ui_svc_setting_get(UI_SET_FM_ID)==1);
    CHECK((cp_flags()&SETTINGS_CP_NO_FM_ID)==0);
    /* "Use anyway" set by app.c's mask, then FM caller ID off: both kept, a power cycle later too */
    settings_t *st=settings_begin(); st->cp_flags|=SETTINGS_CP_ACCEPTED; settings_end(hal_millis());
    CHECK(ui_svc_setting_set(UI_SET_FM_ID,0));
    CHECK(ui_svc_setting_get(UI_SET_FM_ID)==0);
    power_cycle();
    CHECK(ui_svc_setting_get(UI_SET_FM_ID)==0);
    CHECK(cp_flags()==(SETTINGS_CP_ACCEPTED|SETTINGS_CP_NO_FM_ID));
    /* app.c clears "Use anyway" (a CPS write) with its mask: FM caller ID stays off */
    st=settings_begin(); st->cp_flags&=(uint8_t)~SETTINGS_CP_ACCEPTED; settings_end(hal_millis());
    power_cycle();
    CHECK(ui_svc_setting_get(UI_SET_FM_ID)==0 && cp_flags()==SETTINGS_CP_NO_FM_ID);
    /* On again, kept */
    CHECK(ui_svc_setting_set(UI_SET_FM_ID,1));
    power_cycle();
    CHECK(ui_svc_setting_get(UI_SET_FM_ID)==1 && cp_flags()==0);
    /* Off, then a factory reset's defaults: on */
    CHECK(ui_svc_setting_set(UI_SET_FM_ID,0));
    settings_t def;
    settings_defaults(&def);
    CHECK((def.cp_flags&SETTINGS_CP_NO_FM_ID)==0);
    CHECK(sim_flash_violations()==0);
    sim_flash_free();
    printf("# test_fmid_set: FM caller ID setting stored and kept across power cycles: %d failures\n",failures);
    return failures!=0;
}
