/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus satellites: the radio's look angles, range rate and Doppler (radio/sat.c on the generated sat_data.c,
 * i.e. tle.txt through tools/sat_tle.py) for every shipped satellite over two days from Ankara, a line a sample while
 * it is near or above the horizon. tests/plus/sat_doppler_ref.py runs this and checks each line against its own
 * Python SGP4 and frames on the same tle.txt (docs/plus.md). Output:
 *   D catnum t el_deg rate_kms doppler_145.8MHz doppler_downlink doppler_uplink downlink_hz uplink_hz */
#include <stdio.h>
#include "radio/sat.h"

#define LAT	399334000
#define LON	328597000
#define ALT	900

int main(void)
{
	sat_obs_t o;
	sat_observer(LAT, LON, ALT, &o);
	printf("O %d %d %d\n", LAT, LON, ALT);
	for (uint8_t i = 0; i < sat_count; i++) {
		const sat_info_t *s = &sat_table[i];
		sgp4_t p;
		if (sgp4_init(&p, &s->el) != SGP4_OK) {
			printf("E %u\n", (unsigned)s->catnum);
			return 1;
		}
		int32_t t0 = (int32_t)s->el.epoch + 3600;
		for (int32_t t = t0; t < t0 + 2 * 86400; t += 13) {
			sat_look_t l;
			if (sat_eval(&p, s, (double)t, &o, &l) != SGP4_OK) {
				printf("E %u\n", (unsigned)s->catnum);
				return 1;
			}
			if (l.el < -1.0f)
				continue;
			printf("D %u %ld %.4f %.7f %ld %ld %ld %lu %lu\n", (unsigned)s->catnum, (long)t, (double)l.el,
			       (double)l.rate_kms, (long)sat_doppler_rx(145800000u, l.rate_kms),
			       (long)sat_doppler_rx(s->down_hz, l.rate_kms), (long)sat_doppler_tx(s->up_hz, l.rate_kms),
			       (unsigned long)s->down_hz, (unsigned long)s->up_hz);
		}
	}
	return 0;
}
