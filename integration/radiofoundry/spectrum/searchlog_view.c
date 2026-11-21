/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus search log (tools/plus_searchlog.py, docs/plus.md "Search log on the PC"): the debug link sections
 * "p25s" and "dmrs" carry the P25 and DMR searches' state and found lists, so the PC programmer can keep every call a
 * search finds all night (the radio keeps 8). Read only and not counted as a look (p25s_peek, dmrs_peek): a search
 * still stops 5 s after its screen was left. Keys (new keys may be added; the meaning of these stays):
 *   X.on=0|1 X.phase=N X.at=HZ X.lo=HZ X.hi=HZ X.n=N
 *   p25s.fI=HZ,NAC,TG,SRC,CALLS,AGE_S             (I = 0 newest .. n-1)
 *   dmrs.fI=HZ,CC,TSMASK,SYNCMASK,GROUP,ENC,DST,SRC,CALLS,AGE_S   (CC 255 = not named yet)
 * AGE_S: seconds since the entry was last heard. */
#include "config.h"
#include "hal_time.h"
#include "debuglink/debuglink.h"
#include "radio/searchlog_view.h"
#if CONFIG_P25_SEARCH
#include "radio/p25search.h"
#endif
#if CONFIG_DMR_SEARCH
#include "radio/dmrsearch.h"
#endif

static unsigned age_s(uint32_t now, uint32_t heard)
{
	return (unsigned)((now - heard) / 1000u);
}

void searchlog_view_p25(dl_text_t *t)
{
#if CONFIG_P25_SEARCH
	p25s_peek_t p;
	uint32_t now = hal_millis();

	(void)p25s_peek(0u, &p);
	dl_text(t, "p25s.on=%u\np25s.phase=%u\np25s.at=%lu\np25s.lo=%lu\np25s.hi=%lu\np25s.n=%u\n",
		p.phase != P25S_OFF ? 1u : 0u, (unsigned)p.phase, (unsigned long)p.at_hz, (unsigned long)p.lo_hz,
		(unsigned long)p.hi_hz, (unsigned)p.found_n);
	for (unsigned i = 0; i < P25S_FOUND_MAX; i++) {
		if (!p25s_peek(i, &p))
			break;
		dl_text(t, "p25s.f%u=%lu,%u,%u,%lu,%u,%u\n", i, (unsigned long)p.f.hz, (unsigned)p.f.nac,
			(unsigned)p.f.tg, (unsigned long)p.f.src, (unsigned)p.f.calls, age_s(now, p.f.heard_ms));
	}
#else
	dl_text(t, "p25s.built=0\n");
#endif
}

void searchlog_view_dmr(dl_text_t *t)
{
#if CONFIG_DMR_SEARCH
	dmrs_peek_t p;
	uint32_t now = hal_millis();

	(void)dmrs_peek(0u, &p);
	dl_text(t, "dmrs.on=%u\ndmrs.phase=%u\ndmrs.at=%lu\ndmrs.lo=%lu\ndmrs.hi=%lu\ndmrs.n=%u\n",
		p.phase != DMRS_OFF ? 1u : 0u, (unsigned)p.phase, (unsigned long)p.at_hz, (unsigned long)p.lo_hz,
		(unsigned long)p.hi_hz, (unsigned)p.found_n);
	for (unsigned i = 0; i < DMRS_FOUND_MAX; i++) {
		if (!dmrs_peek(i, &p))
			break;
		dl_text(t, "dmrs.f%u=%lu,%u,%u,%u,%u,%u,%lu,%lu,%u,%u\n", i, (unsigned long)p.f.hz, (unsigned)p.f.cc,
			(unsigned)p.f.ts_mask, (unsigned)p.f.sync_mask, p.f.group ? 1u : 0u, p.f.enc ? 1u : 0u,
			(unsigned long)p.f.dst, (unsigned long)p.f.src, (unsigned)p.f.calls, age_s(now, p.f.heard_ms));
	}
#else
	dl_text(t, "dmrs.built=0\n");
#endif
}
