#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "helper.h"
#include "instr.h"

static const char *instrument_names[] = {
	"sine", "square", "triangle", "sawtooth",
	"fm", "white noise", "cubic sin", "hilbert-transformed triangle",
	"nes triangle", "filtered white noise"
};
static const int instrument_amt = sizeof(instrument_names) / sizeof(*instrument_names);
static const char *fx_names[] = {
	"linear fade", "exponential fade", "amplify", "clip"
};
static const int fx_amt = sizeof(fx_names) / sizeof(*fx_names);

static void get_note(char *s, const Editor *editor, int8_t note)
{
	if (note == -128)
	{
		strcpy(s, "none");
		return;
	}
	static const char *sharp[12] = {
		"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
	};
	static const char *flat[12] = {
		"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"
	};
	int name = ((int)note + 129) % 12;
	int oct = ((int)note + 129) / 12 - 6;
	sprintf(s, "%s%d", editor->accidental ? flat[name] : sharp[name], oct);
}

static int cmd_00(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	return snprintf(s, n, "set tick length to %" PRIu32 "us", get_u24(c, 1));
}

static int cmd_01(char *s, int n, const Editor *editor, const uint8_t *c)
{
	char buf[6];
	get_note(buf, editor, get_s8(c, 1));
	uint16_t ticks = get_u16(c, 2);
	return snprintf(s, n, "play %s for %" PRIu16 " %s", buf, ticks, ticks != 1 ? "ticks" : "tick");
}

static int cmd_02(char *s, int n, const Editor *editor, const uint8_t *c)
{
	char buf[6];
	get_note(buf, editor, get_s8(c, 1));
	uint16_t ticks = get_u16(c, 2);
	return snprintf(s, n, "play %s for %" PRIu16 " %s without advancing", buf, ticks, ticks != 1 ? "ticks" : "tick");
}

static int cmd_03(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	switch (get_u8(c, 1))
	{
	case 0:
	{
		uint16_t instrument = get_u16(c, 2);
		return snprintf(s, n, "change instrument to %s",
			instrument < instrument_amt ? instrument_names[instrument] : "a non-existent instrument");
	}
	case 1: return snprintf(s, n, "change attack time to %.3f", (1.0f / 256.0f) * get_u16(c, 2));
	case 2: return snprintf(s, n, "change attack amplitude to %.5f", (1.0f / 65536.0f) * get_u16(c, 2));
	case 3: return snprintf(s, n, "change decay time to %.3f", (1.0f / 256.0f) * get_u16(c, 2));
	case 4: return snprintf(s, n, "change sustain amplitude to %.5f", (1.0f / 65536.0f) * get_u16(c, 2));
	case 5: return snprintf(s, n, "change release time to %.3f", (1.0f / 256.0f) * get_u16(c, 2));
	case 6: return snprintf(s, n, "change lfo frequency to %.3f", (1.0f / 256.0f) * get_u16(c, 2));
	case 7: return snprintf(s, n, "change lfo amplitude to %.5f", (1.0f / 65536.0f) * get_u16(c, 2));
	case 8: return snprintf(s, n, "change instrument volume to %.3f", (1.0f / 256.0f) * get_u16(c, 2));
	case 9: return snprintf(s, n, "change fm ratio to %.4f", (1.0f / 4096.0f) * get_u16(c, 2));
	case 10: return snprintf(s, n, "change fm amplitude to %.4f", (1.0f / 4096.0f) * get_u16(c, 2));
	case 11: return snprintf(s, n, "change square duty cycle to %.5f", (1.0f / 65536.0f) * get_u16(c, 2));
	case 12: return snprintf(s, n, "change pitch bend to %+.3f cents", (100.0f / 65536.0f) * get_s16(c, 2));
	case 13:
	{
		int16_t speed = get_s16(c, 2);
		return snprintf(s, n, "change pitch slide to %+" PRId16 " %s/second", speed, speed == 1 || speed == -1 ? "cent" : "cents");
	}
	case 14: return snprintf(s, n, "change start phase to %.3f degrees", (360.0f / 65536.0f) * get_u16(c, 2));
	case 15: return snprintf(s, n, "change lfo start phase to %.3f degrees", (360.0f / 65536.0f) * get_u16(c, 2));
	case 16:
	{
		uint16_t cut = get_u16(c, 2);
		if (cut == 0) return snprintf(s, n, "change filtered white noise to low-pass only");
		return snprintf(s, n, "change filtered white noise high/low to %.5f", (1.0f / 65536.0f) * cut);
	}
	default: return snprintf(s, n, "change setting 0x%02X to 0x%04X", get_u8(c, 1), get_u16(c, 2));
	}
}

static int cmd_04(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	int32_t dt = get_s24(c, 1);
	int32_t t = dt < 0 ? -dt : dt;
	return snprintf(s, n, "%s by %" PRId32 " %s", dt < 0 ? "rewind" : "seek", t, t != 1 ? "ticks" : "tick");
}

static int cmd_05(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	switch (get_u8(c, 1))
	{
	case 0: return snprintf(s, n, "change linear fade start amplitude to %.4f", (1.0f / 4096.0f) * get_u16(c, 2));
	case 1: return snprintf(s, n, "change linear fade end amplitude to %.4f", (1.0f / 4096.0f) * get_u16(c, 2));
	case 2: return snprintf(s, n, "change exponential fade start amplitude to %.4f", (1.0f / 4096.0f) * get_u16(c, 2));
	case 3: return snprintf(s, n, "change exponential fade end amplitude to %.4f", (1.0f / 4096.0f) * get_u16(c, 2));
	case 4: return snprintf(s, n, "change amplify amplitude to %.4f", (1.0f / 4096.0f) * get_u16(c, 2));
	case 5: return snprintf(s, n, "change clip amplitude to %.4f", (1.0f / 4096.0f) * get_u16(c, 2));
	default: return snprintf(s, n, "change fx setting 0x%02X to 0x%04X", get_u8(c, 1), get_u16(c, 2));
	}
}

static int cmd_06(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	uint8_t fx = get_u8(c, 1);
	uint16_t ticks = get_u16(c, 2);
	return snprintf(s, n, "apply %s for %" PRIu16 " %s",
		fx < fx_amt ? fx_names[fx] : "a non-existent fx", ticks, ticks != 1 ? "ticks" : "tick");
}

static int cmd_07(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	uint8_t fx = get_u8(c, 1);
	uint16_t ticks = get_u16(c, 2);
	return snprintf(s, n, "apply %s for %" PRIu16 " %s without advancing",
		fx < fx_amt ? fx_names[fx] : "a non-existent fx", ticks, ticks != 1 ? "ticks" : "tick");
}

static int cmd_08(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	static const char *op_names[4] = {
		"=", "+=", "-=", "*="
	};
	uint8_t op = get_u8(c, 1);
	uint8_t reg = op & 0x3F;
	op >>= 6;
	return snprintf(s, n, "r%" PRIu8 " %s %" PRId16, reg, op_names[op], get_s16(c, 2));
}

static int cmd_09(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	static const char *cond_names[4] = {
		"is zero", "is not zero", "is positive", "is negative"
	};
	uint8_t cond = get_u8(c, 1);
	uint8_t reg = cond & 0x3F;
	cond >>= 6;
	int16_t dt = get_s16(c, 2);
	int16_t t = dt < 0 ? -dt : dt;
	return snprintf(s, n, "jump %" PRId16 " %s %s if r%" PRIu8 " %s",
		t,
		t != 1 ? "instructions" : "instruction",
		dt < 0 ? "backwards" : "forwards",
		reg, cond_names[cond]);
}

static void get_label_name(char *s, const Editor *editor, uint32_t l)
{
	if (editor->label_mode == 0)
	{
		sprintf(s, "%u", l);
		return;
	}
	static const char table[] = " abcdefghijklmnopqrstuvwxyz";
	for (int i = 4; i >= 0; i--)
	{
		s[i] = table[l % 27];
		l /= 27;
	}
	s[5] = 0;
}

static int cmd_10(char *s, int n, const Editor *editor, const uint8_t *c)
{
	uint32_t l = get_u24(c, 1);
	char label[10];
	get_label_name(label, editor, l);
	return snprintf(s, n, "(%s):", label);
}

static int cmd_11(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	uint32_t packed = get_u24(c, 1);
	uint8_t reg_note = packed & 0x3F;
	uint8_t reg_ticks = (packed >> 6) & 0x3F;
	uint16_t mult = packed >> 12;
	return snprintf(s, n, "play note r%" PRIu8 " for %" PRIu16 "r%" PRIu8 " ticks", reg_note, mult, reg_ticks);
}

static int cmd_12(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	uint32_t packed = get_u24(c, 1);
	uint8_t reg_note = packed & 0x3F;
	uint8_t reg_ticks = (packed >> 6) & 0x3F;
	uint16_t mult = packed >> 12;
	return snprintf(s, n, "play note r%" PRIu8 " for %" PRIu16 "r%" PRIu8 " ticks without advancing", reg_note, mult, reg_ticks);
}

static int cmd_13(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	uint8_t setting = get_u8(c, 1);
	uint16_t packed = get_u16(c, 2);
	uint8_t reg = packed & 0x3F;
	static const uint8_t sh_lut[] = { 4, 8, 12, 16 };
	uint8_t sh = (packed >> 6) & 0x03;
	int8_t div = (packed >> 8) & 0x3F;
	uint8_t sat = (packed >> 14) & 0x01;
	uint8_t sgn = packed >> 15;
	int32_t map_a = 0;
	int32_t map_b = 1 << sh_lut[sh];
	if (sgn)
	{
		map_b >>= 1;
		map_a = -map_b;
	}
	if (sat && sgn)
		return snprintf(s, n, "change setting 0x%02X to clamped_map(r%" PRIu8 ", %" PRId8 ", %" PRId8 ", %" PRId32 ", %" PRId32 ")", setting, reg, -div, div, map_a, map_b);
	if (sat)
		return snprintf(s, n, "change setting 0x%02X to clamped_map(r%" PRIu8 ", 0, %" PRId8 ", %" PRId32 ", %" PRId32 ")", setting, reg, div, map_a, map_b);
	if (sgn)
		return snprintf(s, n, "change setting 0x%02X to map(r%" PRIu8 ", %" PRId8 ", %" PRId8 ", %" PRId32 ", %" PRId32 ")", setting, reg, -div, div, map_a, map_b);
	return snprintf(s, n, "change setting 0x%02X to map(r%" PRIu8 ", 0, %" PRId8 ", %" PRId32 ", %" PRId32 ")", setting, reg, div, map_a, map_b);
}

static int cmd_14(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	uint32_t packed = get_u24(c, 1);
	uint8_t reg = packed & 0x3F;
	int32_t mult = packed >> 6;
	if (mult >= 131072) mult -= 262144;
	return snprintf(s, n, "seek by %" PRId32 "r%hhu", mult, reg);
}

static int cmd_15(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	uint8_t setting = get_u8(c, 1);
	uint16_t packed = get_u16(c, 2);
	uint8_t reg = packed & 0x3F;
	static const uint8_t sh_lut[] = { 4, 8, 12, 16 };
	uint8_t sh = (packed >> 6) & 0x03;
	int8_t div = (packed >> 8) & 0x3F;
	uint8_t sat = (packed >> 14) & 0x01;
	uint8_t sgn = packed >> 15;
	int32_t map_a = 0;
	int32_t map_b = 1 << sh_lut[sh];
	if (sgn)
	{
		map_b >>= 1;
		map_a = -map_b;
	}
	if (sat && sgn)
		return snprintf(s, n, "change fx setting 0x%02X to clamped_map(r%" PRIu8 ", %" PRId8 ", %" PRId8 ", %" PRId32 ", %" PRId32 ")", setting, reg, -div, div, map_a, map_b);
	if (sat)
		return snprintf(s, n, "change fx setting 0x%02X to clamped_map(r%" PRIu8 ", 0, %" PRId8 ", %" PRId32 ", %" PRId32 ")", setting, reg, div, map_a, map_b);
	if (sgn)
		return snprintf(s, n, "change fx setting 0x%02X to map(r%" PRIu8 ", %" PRId8 ", %" PRId8 ", %" PRId32 ", %" PRId32 ")", setting, reg, -div, div, map_a, map_b);
	return snprintf(s, n, "change fx setting 0x%02X to map(r%" PRIu8 ", 0, %" PRId8 ", %" PRId32 ", %" PRId32 ")", setting, reg, div, map_a, map_b);
}

static int cmd_16(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	uint8_t fx = get_u8(c, 1);
	uint16_t packed = get_u16(c, 2);
	uint8_t reg_ticks = packed & 0x3F;
	uint16_t mult = packed >> 6;
	return snprintf(s, n, "apply %s for %" PRIu16 "r%" PRIu8 " ticks",
		fx < fx_amt ? fx_names[fx] : "a non-existent fx", mult, reg_ticks);
}

static int cmd_17(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	uint8_t fx = get_u8(c, 1);
	uint16_t packed = get_u16(c, 2);
	uint8_t reg_ticks = packed & 0x3F;
	uint16_t mult = packed >> 6;
	return snprintf(s, n, "apply %s for %" PRIu16 "r%" PRIu8 " ticks without advancing",
		fx < fx_amt ? fx_names[fx] : "a non-existent fx", mult, reg_ticks);
}

static int cmd_1e(char *s, int n, const Editor *editor, const uint8_t *c)
{
	uint32_t l = get_u24(c, 1);
	char label[10];
	get_label_name(label, editor, l);
	return snprintf(s, n, "jump to (%s)", label);
}

static int cmd_1f(char *s, int n, const Editor *editor, const uint8_t *c)
{
	uint32_t l = get_u24(c, 1);
	char label[10];
	get_label_name(label, editor, l);
	return snprintf(s, n, "call (%s)", label);
}

static int cmd_20(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	uint8_t op = get_u8(c, 1);
	uint16_t val = get_u16(c, 2);
	switch (op)
	{
	case 0:
		if (val == 0) return snprintf(s, n, "push track");
		return snprintf(s, n, "mix down %" PRIu16 " %s", val, val == 1 ? "track" : "tracks");
	case 1:
		if (val == 0) return snprintf(s, n, "push all settings");
		return snprintf(s, n, "pop all settings");
	default: return snprintf(s, n, "unknown stack operation");
	}
}

static int cmd_un(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	return snprintf(s, n, "unk cmd 0x%02X params 0x%02X 0x%02X 0x%02X",
		get_u8(c, 0), get_u8(c, 1), get_u8(c, 2), get_u8(c, 3));
}

static int cmd_ff(char *s, int n, const Editor *editor, const uint8_t *c)
{
	(void)editor;
	(void)c;
	return snprintf(s, n, "return");
}

static int (*cmd[256])(char *, int, const Editor *, const uint8_t *) = {
/*00*/	cmd_00, cmd_01, cmd_02, cmd_03, cmd_04, cmd_05, cmd_06, cmd_07,
	cmd_08, cmd_09, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*10*/	cmd_10, cmd_11, cmd_12, cmd_13, cmd_14, cmd_15, cmd_16, cmd_17,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_1e, cmd_1f,
/*20*/	cmd_20, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*30*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*40*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*50*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*60*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*70*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*80*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*90*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*A0*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*B0*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*C0*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*D0*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*E0*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*F0*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_ff
};

int snfmtinstr(char *dest, int n, const Editor *editor, const ListNode *node)
{
	return cmd[node->e[0]](dest, n, editor, node->e);
}
