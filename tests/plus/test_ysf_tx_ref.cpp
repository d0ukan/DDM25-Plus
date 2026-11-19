// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus YSF Voice FR transmit (src/ysf/ysf_tx.c) against MMDVMHost (G4KLX, GPL-2.0-or-later), compiled on the
// host only (tools/test_ysf_tx_ref.sh). Whole transmissions from our transmitter, every frame given to the reference:
//  - FICH: CYSFFICH::decode reads our fields; its encode() of what it read gives our bytes back (bit for bit)
//  - header / terminator: CYSFPayload::writeHeader with the same CSD1 / CSD2 gives our payload bit for bit;
//    processHeaderData reads our callsign as the source and "**********" as the destination
//  - sub-header (FN 0 / FT 1): CYSFControl's rule (copied as the oracle) calls it the first packet after the header;
//    processVoiceFRModeData accepts the CSD3 CRC and its regeneration leaves the bytes as they were
//  - voice: processVoiceFRModeAudio2 / Audio5 (CAMBEFEC::regenerateIMBE) find 0 errors and change no bit
// The reference sources are not in this tree: the runner compiles them from research/ref.
#include <cstdio>
#include <cstring>
#include "YSFPayload.h"
#include "YSFFICH.h"
extern "C" {
#include "ysf/ysf.h"
#include "ysf/ysf_tx.h"
}

static int failures;
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 31337u;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng >> 8; }

// CYSFControl::writeModem / writeNetwork, case YSF_DT_VOICE_FR_MODE (the oracle): Audio2 for FN 0 / FT 1, else Audio5
static unsigned oracle_count(unsigned fn, unsigned ft) { return (fn == 0U && ft == 1U) ? 2U : 5U; }

static void random_u(uint16_t u[8])
{
	for (unsigned i = 0U; i < 8U; i++)
		u[i] = (uint16_t)(rnd() & ((1U << (i < 4U ? 12U : i < 7U ? 11U : 7U)) - 1U));
}

static unsigned frames_seen, headers, subheaders, voices, terminators;

static void check_frame(const uint8_t ours[YSF_FRAME_BYTES], unsigned index, const char *src10, unsigned dgid)
{
	frames_seen++;
	static const uint8_t sync[5] = {0xD4U, 0x71U, 0xC9U, 0x63U, 0x4DU};
	CHECK(std::memcmp(ours, sync, 5U) == 0);
	CYSFFICH fich;
	CHECK(fich.decode(ours));
	uint8_t again[YSF_FRAME_BYTES];
	std::memcpy(again, ours, sizeof again);
	std::memset(again + 5, 0, 25U);
	fich.encode(again);
	CHECK(std::memcmp(again, ours, YSF_FRAME_BYTES) == 0);
	CHECK(fich.getDT() == YSF_DT_VOICE_FR && fich.getCM() == 0U && fich.getBN() == 0U && fich.getBT() == 0U);
	CHECK(fich.getMR() == 0U && fich.getDev() == (YSF_TX_LOW_DEV != 0) && fich.getDGId() == dgid);
	unsigned fi = fich.getFI(), fn = fich.getFN(), ft = fich.getFT();
	if (fi == YSF_FI_HEADER || fi == YSF_FI_TERMINATOR) {
		CHECK((fi == YSF_FI_HEADER) == (index == 0U));
		fi == YSF_FI_HEADER ? headers++ : terminators++;
		CHECK(fn == 0U && ft == 1U);
		uint8_t csd1[20], csd2[20], ref[YSF_FRAME_BYTES];
		std::memset(csd1, '*', 10);
		std::memcpy(csd1 + 10, src10, 10);
		std::memset(csd2, ' ', 20);
		std::memcpy(ref, ours, sizeof ref);
		std::memset(ref + 30, 0, 90);
		CYSFPayload writer;
		writer.writeHeader(ref, csd1, csd2);
		CHECK(std::memcmp(ref, ours, YSF_FRAME_BYTES) == 0);
		CYSFPayload reader;
		std::memcpy(ref, ours, sizeof ref);
		CHECK(reader.processHeaderData(ref));
		CHECK(reader.getSource() != nullptr && std::memcmp(reader.getSource(), src10, 10) == 0);
		CHECK(reader.getDest() != nullptr && std::memcmp(reader.getDest(), "**********", 10) == 0);
		CHECK(std::memcmp(ref, ours, YSF_FRAME_BYTES) == 0);	// regenerated as it was
		return;
	}
	CHECK(fi == YSF_FI_COMMS);
	uint8_t copy[YSF_FRAME_BYTES];
	std::memcpy(copy, ours, sizeof copy);
	CYSFPayload payload;
	if (oracle_count(fn, ft) == 2U) {
		CHECK(index == 1U);
		subheaders++;
		CHECK(payload.processVoiceFRModeData(copy));
		CHECK(payload.processVoiceFRModeAudio2(copy) == 0U);
	} else {
		CHECK(index >= 2U && fn == 0U && ft == 0U);
		voices++;
		CHECK(payload.processVoiceFRModeAudio5(copy) == 0U);
	}
	CHECK(std::memcmp(copy, ours, YSF_FRAME_BYTES) == 0);
}

// One transmission of `talk` IMBE frames, frame by frame through check_frame
static void transmission(const char *call, unsigned dgid, unsigned talk)
{
	static uint8_t frame[YSF_FRAME_BYTES], out[YSF_FRAME_BYTES];
	char src10[11];
	std::snprintf(src10, sizeof src10, "%-10s", call);
	ysf_tx_t t;
	ysf_tx_begin(&t, frame, call, (uint8_t)dgid);
	unsigned spoken = 0U, have = 0U, index = 0U, guard = 0U;
	while (!ysf_tx_done(&t) && guard++ < 100000U) {
		have += ysf_tx_take(&t, out, have, YSF_FRAME_BITS - have);
		if (have == YSF_FRAME_BITS) {
			check_frame(out, index++, src10, dgid);
			have = 0U;
		}
		if (ysf_tx_need_voice(&t)) {
			uint16_t u[8];
			if (spoken < talk) {
				random_u(u);
				spoken++;
			} else if (!t.closing) {
				ysf_tx_close(&t);
				continue;
			} else {
				std::memset(u, 0, sizeof u);
			}
			ysf_tx_voice(&t, u);
		}
	}
	CHECK(ysf_tx_done(&t) && have == 0U && index == t.frames);
}

int main()
{
	static const char *calls[] = {"TA1ABC", "TA2XYZ", "K1A", "TA1ABCDEFG", "DL1ABC", "N0CALL"};
	for (unsigned round = 0U; round < 120U; round++)
		transmission(calls[round % 6U], rnd() % 128U, 1U + rnd() % 60U);
	CHECK(headers == 120U && terminators == 120U && subheaders == 120U && voices > 120U);
	std::printf("YSF VW transmit vs MMDVMHost: %u frames (%u voice), %u checks, %d failures\n", frames_seen, voices,
		    checks, failures);
	return failures ? 1 : 0;
}
