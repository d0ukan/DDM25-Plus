/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Settings > Talk & listen > Watch channels (UI_SET_MWATCH) and the ring's pane 1 letter (UI_SET_MW_P1)
 * through the actual UI storage service and settings store on the DM32 flash map (sim_flash): 2 (dual watch) in a new
 * radio and after a factory reset, 2..6 kept across power cycles, stored in settings_t.dual_state's spare bits 2-7
 * next to dual watch's own (on at power off, B selected), which neither disturbs. Built by tools/test_classic.py like
 * tests/plus/test_fmid_set.c, with -DCONFIG_MULTI_WATCH=1. */
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

static uint8_t dual_state(void)
{
    settings_t st;
    settings_read(&st);
    return st.dual_state;
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
    /* A new radio: dual watch's 2, nothing stored */
    CHECK(ui_svc_setting_get(UI_SET_MWATCH)==2 && ui_svc_setting_get(UI_SET_MW_P1)==0 && dual_state()==0);
    /* Each count kept across a power cycle, in bits 2-4 only */
    for (int k=0;k<5;k++) {
        int n=3+k<=6 ? 3+k : 2;   /* 3, 4, 5, 6, then 2 again (a first save of the default writes nothing) */
        CHECK(ui_svc_setting_set(UI_SET_MWATCH,n));
        power_cycle();
        CHECK(ui_svc_setting_get(UI_SET_MWATCH)==n);
        CHECK(dual_state()==(uint8_t)((n-2)<<SETTINGS_MW_N_SHIFT));
    }
    /* Out of range: held to 2..6 */
    CHECK(ui_svc_setting_set(UI_SET_MWATCH,9) && ui_svc_setting_get(UI_SET_MWATCH)==6);
    CHECK(ui_svc_setting_set(UI_SET_MWATCH,0) && ui_svc_setting_get(UI_SET_MWATCH)==2);
    /* 4 channels, D in pane 1, then dual watch saves "on, B selected" (its pane 1): all three kept side by side */
    CHECK(ui_svc_setting_set(UI_SET_MWATCH,4) && ui_svc_setting_set(UI_SET_MW_P1,3));
    CHECK(ui_svc_setting_set(UI_SET_DUAL_B,12|UI_DUAL_ON|UI_DUAL_B_SEL));
    power_cycle();
    CHECK(ui_svc_setting_get(UI_SET_MWATCH)==4 && ui_svc_setting_get(UI_SET_MW_P1)==3);
    CHECK(ui_svc_setting_get(UI_SET_DUAL_B)==(12|UI_DUAL_ON|UI_DUAL_B_SEL));
    CHECK(dual_state()==(SETTINGS_DUAL_ON|SETTINGS_DUAL_B_SEL|(2u<<SETTINGS_MW_N_SHIFT)|(3u<<SETTINGS_MW_P1_SHIFT)));
    /* The watch stopped (dual watch saves "off"): the count and the letter stay */
    CHECK(ui_svc_setting_set(UI_SET_DUAL_B,12));
    power_cycle();
    CHECK(ui_svc_setting_get(UI_SET_MWATCH)==4 && ui_svc_setting_get(UI_SET_MW_P1)==3 && ui_svc_setting_get(UI_SET_DUAL_B)==12);
    /* A factory reset's defaults: dual watch's 2 */
    settings_t def;
    settings_defaults(&def);
    CHECK((def.dual_state&(SETTINGS_MW_N_MASK|SETTINGS_MW_P1_MASK))==0);
    CHECK(sim_flash_violations()==0);
    sim_flash_free();
    printf("# test_mwatch_set: Watch channels stored in dual_state's spare bits, kept across power cycles: %d failures\n",failures);
    return failures!=0;
}
