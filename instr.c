#include <stdint.h>
#include <string.h>
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
	return snprintf(s, n, "set tick length to %uus", get_u24(c, 1));
}

static int cmd_01(char *s, int n, const Editor *editor, const uint8_t *c)
{
	char buf[6];
	get_note(buf, editor, get_s8(c, 1));
	uint16_t ticks = get_u16(c, 2);
	return snprintf(s, n, "play %s for %d %s", buf, ticks, ticks != 1 ? "ticks" : "tick");
}

static int cmd_02(char *s, int n, const Editor *editor, const uint8_t *c)
{
	char buf[6];
	get_note(buf, editor, get_s8(c, 1));
	uint16_t ticks = get_u16(c, 2);
	return snprintf(s, n, "play %s for %d %s without advancing", buf, ticks, ticks != 1 ? "ticks" : "tick");
}

static int cmd_03(char *s, int n, const Editor *editor, const uint8_t *c)
{
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
		return snprintf(s, n, "change pitch slide to %+d %s/second", speed, speed == 1 || speed == -1 ? "cent" : "cents");
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
	int32_t dt = get_s24(c, 1);
	int32_t t = dt < 0 ? -dt : dt;
	return snprintf(s, n, "%s by %d %s", dt < 0 ? "rewind" : "seek", t, t != 1 ? "ticks" : "tick");
}

static int cmd_05(char *s, int n, const Editor *editor, const uint8_t *c)
{
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
	uint8_t fx = get_u8(c, 1);
	uint16_t ticks = get_u16(c, 2);
	return snprintf(s, n, "apply %s for %d %s",
		fx < fx_amt ? fx_names[fx] : "a non-existent fx", ticks, ticks != 1 ? "ticks" : "tick");
}

static int cmd_07(char *s, int n, const Editor *editor, const uint8_t *c)
{
	uint8_t fx = get_u8(c, 1);
	uint16_t ticks = get_u16(c, 2);
	return snprintf(s, n, "apply %s for %d %s without advancing",
		fx < fx_amt ? fx_names[fx] : "a non-existent fx", ticks, ticks != 1 ? "ticks" : "tick");
}

static int cmd_08(char *s, int n, const Editor *editor, const uint8_t *c)
{
	static const char *op_names[4] = {
		"=", "+=", "-=", "*="
	};
	uint8_t op = get_u8(c, 1);
	uint8_t reg = op & 0x3F;
	op >>= 6;
	return snprintf(s, n, "r%hhu %s %hd", reg, op_names[op], get_s16(c, 2));
}

static int cmd_09(char *s, int n, const Editor *editor, const uint8_t *c)
{
	static const char *cond_names[4] = {
		"is zero", "is not zero", "is positive", "is negative"
	};
	uint8_t cond = get_u8(c, 1);
	uint8_t reg = cond & 0x3F;
	cond >>= 6;
	int16_t dt = get_s16(c, 2);
	int16_t t = dt < 0 ? -dt : dt;
	return snprintf(s, n, "jump %hu %s %s if r%hhu %s",
		t,
		t != 1 ? "instructions" : "instruction",
		dt < 0 ? "backwards" : "forwards",
		reg, cond_names[cond]);
}

static int cmd_un(char *s, int n, const Editor *editor, const uint8_t *c)
{
	return snprintf(s, n, "unk cmd 0x%02X params 0x%02X 0x%02X 0x%02X",
		get_u8(c, 0), get_u8(c, 1), get_u8(c, 2), get_u8(c, 3));
}

static int cmd_ff(char *s, int n, const Editor *editor, const uint8_t *c)
{
	return snprintf(s, n, "exit");
}

static int (*cmd[256])(char *, int, const Editor *, const uint8_t *) = {
/*00*/	cmd_00, cmd_01, cmd_02, cmd_03, cmd_04, cmd_05, cmd_06, cmd_07,
	cmd_08, cmd_09, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*10*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
/*20*/	cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un, cmd_un,
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
