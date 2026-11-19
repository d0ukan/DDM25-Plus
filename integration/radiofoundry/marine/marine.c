/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus marine VHF channel table (marine.h), written from ITU Radio Regulations Appendix 18 (Rev. WRC-19), "Table
 * of transmitting frequencies in the VHF maritime mobile band": channel designator, ship station and coast station
 * transmitting frequency, and its notes on use. Two frequencies 4.6 MHz apart: the ship sends on the lower, the coast
 * station on the upper. The four digit channels of Appendix 18 (1019, 2019, ...: one side of a duplex channel used
 * alone, known in some administrations as 19A / 19B) are not listed: their frequencies are the two shown for the
 * channel. AIS 1 and AIS 2 (161.975 / 162.025 MHz) are the channels called 87B and 88B. Not listed either: channel
 * 2006 (160.900 MHz, Appendix 18 note r) and the national 12.5 kHz interleaved channels. */
#include "config.h"
#if CONFIG_MARINE
#include <string.h>
#include "radio/marine.h"

#define F(khz)	(uint16_t)(((khz) - 156000) * 2 / 25)	/* kHz -> 12.5 kHz steps above 156.000 MHz */
#define NONE	MARINE_NONE

/* In channel number order */
const marine_ch_t marine_table[] = {
	{ "01", F(156050), F(160650), MAR_DUPLEX },
	{ "02", F(156100), F(160700), MAR_DUPLEX },
	{ "03", F(156150), F(160750), MAR_DUPLEX },
	{ "04", F(156200), F(160800), MAR_DUPLEX },
	{ "05", F(156250), F(160850), MAR_DUPLEX },
	{ "06", F(156300), NONE, MAR_SAR },
	{ "07", F(156350), F(160950), MAR_DUPLEX },
	{ "08", F(156400), NONE, MAR_INTERSHIP },
	{ "09", F(156450), F(156450), MAR_SIMPLEX },
	{ "10", F(156500), F(156500), MAR_SIMPLEX },
	{ "11", F(156550), F(156550), MAR_SIMPLEX },
	{ "12", F(156600), F(156600), MAR_SIMPLEX },
	{ "13", F(156650), F(156650), MAR_BRIDGE },
	{ "14", F(156700), F(156700), MAR_SIMPLEX },
	{ "15", F(156750), F(156750), MAR_ONBOARD },
	{ "16", F(156800), F(156800), MAR_DISTRESS },
	{ "17", F(156850), F(156850), MAR_ONBOARD },
	{ "18", F(156900), F(161500), MAR_DUPLEX },
	{ "19", F(156950), F(161550), MAR_DUPLEX },
	{ "20", F(157000), F(161600), MAR_DUPLEX },
	{ "21", F(157050), F(161650), MAR_DUPLEX },
	{ "22", F(157100), F(161700), MAR_DUPLEX },
	{ "23", F(157150), F(161750), MAR_DUPLEX },
	{ "24", F(157200), F(161800), MAR_DUPLEX },
	{ "25", F(157250), F(161850), MAR_DUPLEX },
	{ "26", F(157300), F(161900), MAR_DUPLEX },
	{ "27", F(157350), F(161950), MAR_DUPLEX },
	{ "28", F(157400), F(162000), MAR_DUPLEX },
	{ "60", F(156025), F(160625), MAR_DUPLEX },
	{ "61", F(156075), F(160675), MAR_DUPLEX },
	{ "62", F(156125), F(160725), MAR_DUPLEX },
	{ "63", F(156175), F(160775), MAR_DUPLEX },
	{ "64", F(156225), F(160825), MAR_DUPLEX },
	{ "65", F(156275), F(160875), MAR_DUPLEX },
	{ "66", F(156325), F(160925), MAR_DUPLEX },
	{ "67", F(156375), F(156375), MAR_SIMPLEX },
	{ "68", F(156425), F(156425), MAR_SIMPLEX },
	{ "69", F(156475), F(156475), MAR_SIMPLEX },
	{ "70", F(156525), F(156525), MAR_DSC },
	{ "71", F(156575), F(156575), MAR_SIMPLEX },
	{ "72", F(156625), NONE, MAR_INTERSHIP },
	{ "73", F(156675), F(156675), MAR_SIMPLEX },
	{ "74", F(156725), F(156725), MAR_SIMPLEX },
	{ "75", F(156775), F(156775), MAR_NAV },
	{ "76", F(156825), F(156825), MAR_NAV },
	{ "77", F(156875), NONE, MAR_INTERSHIP },
	{ "78", F(156925), F(161525), MAR_DUPLEX },
	{ "79", F(156975), F(161575), MAR_DUPLEX },
	{ "80", F(157025), F(161625), MAR_DUPLEX },
	{ "81", F(157075), F(161675), MAR_DUPLEX },
	{ "82", F(157125), F(161725), MAR_DUPLEX },
	{ "83", F(157175), F(161775), MAR_DUPLEX },
	{ "84", F(157225), F(161825), MAR_DUPLEX },
	{ "85", F(157275), F(161875), MAR_DUPLEX },
	{ "86", F(157325), F(161925), MAR_DUPLEX },
	{ "87", F(157375), F(157375), MAR_SIMPLEX },
	{ "88", F(157425), F(157425), MAR_SIMPLEX },
	{ "87B", F(161975), F(161975), MAR_AIS },	/* AIS 1 */
	{ "88B", F(162025), F(162025), MAR_AIS },	/* AIS 2 */
};
const uint8_t marine_count = (uint8_t)(sizeof marine_table / sizeof marine_table[0]);

uint32_t marine_ship_hz(const marine_ch_t *c)
{
	return MARINE_BASE_HZ + (uint32_t)c->ship * MARINE_STEP_HZ;
}

uint32_t marine_coast_hz(const marine_ch_t *c)
{
	return c->coast == MARINE_NONE ? 0u : MARINE_BASE_HZ + (uint32_t)c->coast * MARINE_STEP_HZ;
}

bool marine_listen_hz(const marine_ch_t *c, uint32_t *hz)
{
	if (c->use == MAR_DSC || c->use == MAR_AIS)
		return false;
	*hz = c->coast != MARINE_NONE ? marine_coast_hz(c) : marine_ship_hz(c);
	return true;
}

bool marine_hz(uint32_t hz)
{
	if (hz + MARINE_TOL_HZ <= MARINE_BASE_HZ || hz >= MARINE_BASE_HZ + 6100000u)
		return false;	/* below 156 MHz or above 162.1 MHz: none of them */
	uint32_t off = hz + MARINE_STEP_HZ / 2u - MARINE_BASE_HZ;
	uint32_t step = off / MARINE_STEP_HZ, near = MARINE_BASE_HZ + step * MARINE_STEP_HZ;
	if ((hz > near ? hz - near : near - hz) >= MARINE_TOL_HZ)
		return false;
	for (uint8_t i = 0; i < marine_count; i++)
		if (marine_table[i].ship == step || marine_table[i].coast == step)
			return true;
	return false;
}

int marine_find(const char *name)
{
	for (uint8_t i = 0; i < marine_count; i++) {
		const char *a = marine_table[i].name, *b = name;
		while (*a && *b && (*a == *b || (*a == 'B' && *b == 'b')))
			a++, b++;
		if (*a == 0 && *b == 0)
			return i;
	}
	return -1;
}
#endif
