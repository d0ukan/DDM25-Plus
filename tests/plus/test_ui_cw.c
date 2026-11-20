/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > CW decoder and CW practice through the real UI in the upstream simulator (-DCONFIG_MSG_RX=1
 * -DCONFIG_CW_RX=1): the Tools entry, the decoder screen (speed, tone, mark lamp, the status lines, the text in lines
 * scrolling up, OK clears, # to practice, the decoder wanted only while it is on top), and the practice page (the
 * settings, a group or a character asked of the radio task with the speed and tone set, the answer typed with the
 * keypad letters, the score, * again, # reset, Characters mode's next by itself, a refusal, Back), with the layout check
 * on both and no TX request anywhere (sim tx count 0).
 *
 * ui_msg.c and ui_cw.c are included here (their statics start each flow afresh); the radio task's side of the
 * decoder (radio_msg_*, radio_cw_*) is a fake the test sets; cw_train.c is the real one, the test takes its requests
 * as the radio task would. */
#define main upstream_ui_main
#include "uiFlowTest.c"
#undef main
#include "ui/ui_msg.c"
#include "ui/ui_scratch.c"	/* ui_msg.c's page buffer (DDM25 Plus round 13) */
#include "ui/ui_cw.c"

/* --- the radio task's side, faked --- */
static uint8_t f_want, f_status = RADIO_MSG_ST_ON;
static uint32_t f_seq;
static radio_cw_info_t f_info;
static char f_text[400];
static uint32_t f_base;		/* the number of f_text[0] */
static int f_clears, f_wanted_cw;

void radio_msg_want(uint8_t kind)
{
	f_want = kind;
	f_wanted_cw += kind == RADIO_MSG_CW;
}
uint8_t radio_msg_status(void) { return f_status; }
uint32_t radio_msg_seq(void) { return f_seq; }
uint8_t radio_msg_count(uint8_t kind) { (void)kind; return 0; }
const radio_pager_msg_t *radio_msg_pager(uint8_t i) { (void)i; return NULL; }
const radio_acars_msg_t *radio_msg_acars(uint8_t i) { (void)i; return NULL; }

void radio_cw_info(radio_cw_info_t *out) { *out = f_info; }
uint32_t radio_cw_seq(void) { return f_seq; }
uint32_t radio_cw_written(void) { return f_base + (uint32_t)strlen(f_text); }
uint32_t radio_cw_first(void)
{
	uint32_t w = radio_cw_written();
	uint32_t f = w > RADIO_CW_TEXT ? w - RADIO_CW_TEXT : 0;
	return f > f_base ? f : f_base;
}
uint16_t radio_cw_copy(uint32_t from, char *out, uint16_t n)
{
	uint16_t k = 0;
	uint32_t f = radio_cw_first(), w = radio_cw_written();
	if (from < f)
		from = f;
	for (; k < n && from + k < w; k++)
		out[k] = f_text[from + k - f_base];
	out[k] = 0;
	return k;
}
void radio_cw_clear(void)
{
	f_clears++;
	f_base += (uint32_t)strlen(f_text);
	f_text[0] = 0;
	f_seq++;
}

static void fake_reset(void)
{
	f_want = 0;
	f_status = RADIO_MSG_ST_ON;
	f_seq = 1;
	memset(&f_info, 0, sizeof f_info);
	f_info.on = true;
	f_text[0] = 0;
	f_base = 0;
	f_clears = f_wanted_cw = 0;
	/* ui_cw.c's practice state, as after power-on */
	t_chars = 0;
	t_level = CW_TRAIN_LEVEL_MIN;
	t_wpm = 20;
	t_hz = 600;
	t_row = 0;
	t_phase = PH_IDLE;
	t_n = t_m = 0;
	s_right = s_sent = 0;
	t_rng = 0;
	cw_play_t p;
	while (cw_train_take(&p)) {
	}
}

/* --- the requests the radio task would take --- */
static cw_play_t taken;
static int takes;
static bool take(void)
{
	if (!cw_train_take(&taken))
		return false;
	takes++;
	return true;
}

/* The keys that type c with the keypad letters (multi-tap), then "right" to end it */
static void keys_for(char c, char *out, size_t cap)
{
	static const char *const sets[10] = { "0", ".,?/=1", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7", "TUV8",
					      "WXYZ9" };
	out[0] = 0;
	for (int k = 0; k < 10; k++) {
		const char *p = strchr(sets[k], c);
		if (p == NULL)
			continue;
		for (long i = 0; i <= p - sets[k]; i++) {
			size_t n = strlen(out);
			snprintf(out + n, cap - n, "%d ", k);
		}
		size_t n = strlen(out);
		snprintf(out + n, cap - n, "right ");
		return;
	}
}

static void keys_for_text(const char *t, uint8_t len, char *out, size_t cap)
{
	out[0] = 0;
	for (uint8_t i = 0; i < len; i++) {
		char one[64];
		keys_for(t[i], one, sizeof one);
		size_t n = strlen(out);
		snprintf(out + n, cap - n, "%s", one);
	}
}

#define GEOM "240x320i10x5"
#define TOOLS_CW "ok 6 up up ok "	/* Tools: up wraps to the last (Battery), CW decoder before it */
#define PRACTICE TOOLS_CW "ctx "

static int cw_fails;

/* The decoder screen's last text line as drawn (character k on line k / columns), for an expect (spaces as _) */
static const char *last_line(void)
{
	static char out[64];
	unsigned cols = (unsigned)(gfx_width() / ui_layout.small->w);
	cols = cols > LINE_MAX ? LINE_MAX : cols;
	uint32_t w = radio_cw_written(), from = (w - 1u) / cols * cols;
	radio_cw_copy(from, out, (uint16_t)cols);
	for (char *c = out; *c; c++)
		if (*c == ' ')
			*c = '_';
	return out;
}

static void expect(const char *name, const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL CW %s: %s\n", name, err);
		fails++;
		cw_fails++;
	}
	flows_run++;
}

static void begin(void)
{
	fake_reset();
	sim_reset(sim_geom_find(GEOM), SIM_MAP_M);
	takes = 0;
}

int main(int argc, char **argv)
{
	if (argc > 1) {		/* probe: run a script, print the screen's texts */
		char err[300];
		begin();
		f_info.wpm = 18;
		f_info.tone_hz = 712;
		strcpy(f_text, "CQ CQ DE TA1ABC TA1ABC K");
		sim_run(argv[1], err, sizeof err);
		ui_app_status_t st;
		ui_app_status(&st);
		printf("screen=%s focus=%d toast=%s banner=%s err=%s\n", st.screen, (int)st.focus, st.toast, st.banner, err);
		sim_run("expect:shows=@@@", err, sizeof err);
		printf("%s\n", err);
		if (argc > 2)
			sim_save_png(argv[2], 2);
		return 0;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	char script[1024], kseq[512];
#define CHECKC(c, name) do { flows_run++; if (!(c)) { printf("FAIL CW %s: %s\n", name, #c); fails++; cw_fails++; } } \
	while (0)

	/* The Tools entry: the last tool; the decoder wanted only while its screen is on top */
	begin();
	expect("tools entry", TOOLS_CW "wait:300 expect:screen=cw expect:shows=CW_decoder expect:tx=0");
	CHECKC(f_want == RADIO_MSG_CW && f_wanted_cw > 0, "wanted on top");
	expect("back leaves", "back wait:300 expect:screen=tools");
	CHECKC(f_want == 0, "not wanted after Back");

	/* What it shows: speed, tone, the text; the status lines */
	begin();
	f_info.wpm = 18;
	f_info.tone_hz = 712;
	f_info.key = true;
	strcpy(f_text, "CQ CQ DE TA1ABC TA1ABC K");
	expect("decoding", TOOLS_CW "wait:300 expect:shows=18_WPM__712_Hz expect:shows=Decoding "
	       "expect:shows=CQ_CQ_DE_TA1ABC_TA1ABC_K");
	f_info.wpm = 0;
	f_info.tone_hz = 0;
	f_seq++;
	expect("listening", "wait:300 expect:shows=--_WPM__---_Hz expect:shows=Listening:_MCW_tone_400-1000_Hz");
	f_status = RADIO_MSG_ST_WAIT;
	f_seq++;
	expect("waiting", "wait:300 expect:shows=Waiting_for_receive_audio");
	f_status = RADIO_MSG_ST_OFF;	/* the simulator's channel is DMR */
	f_seq++;
	expect("not here", "wait:300 expect:shows=CW:_tune_an_FM_or_AM_channel");
	f_info.nomem = true;
	f_seq++;
	expect("no heap", "wait:300 expect:shows=Not_enough_memory_for_CW");

	/* A long text: the newest lines, the oldest scrolled away; new characters arrive at the end */
	begin();
	f_info.wpm = 25;
	f_info.tone_hz = 650;
	for (int i = 0; i < 40; i++)
		strcat(f_text, i == 0 ? "FIRSTWORD " : i == 39 ? "LASTWORD" : "PARIS ");
	snprintf(script, sizeof script, TOOLS_CW "wait:300 expect:hides=FIRSTWORD expect:shows=%s", last_line());
	expect("scrolled", script);
	strcat(f_text, " NEW");
	f_seq++;
	snprintf(script, sizeof script, "wait:300 expect:shows=%s", last_line());
	expect("new text", script);
	{
		char issues[SIM_LAYOUT_MAX][160];
		int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
		for (int k = 0; k < n; k++)
			printf("FAIL CW layout decoder: %s\n", issues[k]);
		fails += n != 0;
		cw_fails += n != 0;
	}
	/* OK clears */
	expect("clear", "ok wait:300 expect:hides=NEW expect:screen=cw");
	CHECKC(f_clears == 1, "OK clears once");

	/* Practice: # opens it; the decoder is not wanted under it */
	begin();
	expect("practice", PRACTICE "wait:300 expect:screen=cw-practice expect:shows=Groups expect:shows=2__KM "
	       "expect:shows=20_WPM expect:shows=600_Hz expect:shows=OK_plays_the_first_one expect:shows=Score_0/0 "
	       "expect:tx=0");
	CHECKC(f_want == 0, "decoder not wanted under the practice page");
	/* OK plays a group of five of K M at 20 WPM, 600 Hz */
	expect("play", "ok expect:shows=Listen...");
	CHECKC(take() && taken.len == CW_TRAIN_GROUP && taken.wpm == 20 && taken.hz == 600 &&
	       strspn(taken.text, "KM") >= CW_TRAIN_GROUP, "a group of K M asked for");
	cw_train_done(true);
	uint32_t ms = cw_train_ms(taken.text, taken.len, taken.wpm) + CW_TRAIN_LEAD_MS + 200u;
	snprintf(script, sizeof script, "wait:%u expect:shows=Type_what_you_heard,_OK", (unsigned)ms);
	expect("answer time", script);
	/* the right answer typed with the keypad letters */
	keys_for_text(taken.text, taken.len, kseq, sizeof kseq);
	snprintf(script, sizeof script, "%s ok expect:shows=Right: expect:shows=Score_5/5__100_%% expect:tx=0", kseq);
	expect("right answer", script);
	/* the next: a wrong letter and a missing one */
	expect("next", "ok");
	CHECKC(take(), "OK with nothing typed plays the next");
	cw_train_done(true);
	char wrong[CW_TRAIN_MAX];
	memcpy(wrong, taken.text, taken.len);
	wrong[0] = wrong[0] == 'K' ? 'M' : 'K';
	keys_for_text(wrong, (uint8_t)(taken.len > 0 ? taken.len - 1u : 0u), kseq, sizeof kseq);
	snprintf(script, sizeof script, "%s ok expect:shows=Sent: expect:shows=Score_8/10__80_%%", kseq);
	expect("wrong answer", script);
	/* OK held plays it again: the same text asked for again */
	char last[CW_TRAIN_MAX];
	memcpy(last, taken.text, taken.len);
	expect("again", "long:ok expect:shows=Listen...");
	CHECKC(take() && memcmp(taken.text, last, taken.len) == 0, "OK held asks for the same again");
	cw_train_done(true);
	/* typing: Back and Left delete, Right ends a letter (5 5 = K, then 5 again starts a new J) */
	/* (the cursor is an underscore: an expect's _ is a space, so the checks stop before it) */
	expect("typing", "5 5 right 5 expect:shows=>_KJ left expect:shows=>_K expect:hides=>_KJ back expect:hides=>_K "
	       "expect:screen=cw-practice");
	/* # sets the score to zero */
	expect("reset", "ctx expect:toast=Score_reset expect:shows=Score_0/0");
	/* Back with nothing typed leaves to the decoder */
	expect("back to decoder", "back wait:300 expect:screen=cw");
	CHECKC(f_want == RADIO_MSG_CW, "the decoder wanted again");

	/* Settings: Koch 5, 19 WPM, 650 Hz, asked for with them */
	begin();
	expect("settings", PRACTICE "down right right right expect:shows=5__KMURE down left expect:shows=19_WPM "
	       "down right expect:shows=650_Hz down expect:shows=Groups");
	expect("settings play", "ok");
	CHECKC(take() && taken.wpm == 19 && taken.hz == 650 && strspn(taken.text, "KMURE") >= CW_TRAIN_GROUP,
	       "the settings asked for");
	cw_train_done(true);
	expect("settings ends", "down down down left left left left left left left left left left left left left "
	       "expect:shows=400_Hz");

	/* Characters mode: one at a time, its pattern shown after the check, the next by itself a second later */
	begin();
	expect("chars", PRACTICE "right expect:shows=Characters ok");
	CHECKC(take() && taken.len == 1, "one character");
	cw_train_done(true);
	keys_for_text(taken.text, 1, kseq, sizeof kseq);
	snprintf(script, sizeof script, "wait:400 %s ok expect:shows=Right:_%c__%s expect:shows=Score_1/1__100_%%", kseq,
		 taken.text[0], taken.text[0] == 'K' ? "-.-" : "--");
	expect("chars right", script);
	{
		char issues[SIM_LAYOUT_MAX][160];
		int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
		for (int k = 0; k < n; k++)
			printf("FAIL CW layout practice: %s\n", issues[k]);
		fails += n != 0;
		cw_fails += n != 0;
	}
	int before = takes;
	expect("chars next", "wait:1300 expect:shows=Listen...");
	CHECKC(take() && takes == before + 1 && taken.len == 1, "the next one by itself");
	cw_train_done(true);

	/* Refused (the radio transmits): back to idle with a toast */
	begin();
	expect("refused", PRACTICE "ok");
	CHECKC(take(), "asked");
	cw_train_done(false);
	expect("refused toast", "wait:300 expect:toast=Not_while_transmitting expect:shows=OK_plays_the_first_one "
	       "expect:tx=0");

	/* Practice with an answer typed: the layout */
	begin();
	expect("layout typing", PRACTICE "ok 2 2 right 6 6 6 expect:shows=OK_check");
	{
		char issues[SIM_LAYOUT_MAX][160];
		int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
		for (int k = 0; k < n; k++)
			printf("FAIL CW layout typing: %s\n", issues[k]);
		fails += n != 0;
		cw_fails += n != 0;
	}
	take();
	cw_train_done(true);
	printf("Plus CW decoder and practice screens: %d checks, %d failures\n", flows_run, cw_fails);
	return cw_fails != 0;
}
