#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <wordexp.h>
#include <math.h>
#include "render_internal.h"
#include "vector.h"
#include "map.h"
#include "helper.h"

define_vector(vector_u64, uint64_t);
define_vector(vector_f, float);

enum Instrument
{
	INSTRUMENT_SINE,
	INSTRUMENT_SQUARE,
	INSTRUMENT_TRIANGLE,
	INSTRUMENT_SAWTOOTH,
	INSTRUMENT_FM,
	INSTRUMENT_NOISE,
	INSTRUMENT_CUBIC,
	INSTRUMENT_HILBERT_TRI,
	INSTRUMENT_NES_TRI,
	INSTRUMENT_COUNT
};

enum Setting
{
	SETTING_INSTRUMENT,
	SETTING_ATTACK_TIME,
	SETTING_ATTACK_AMP,
	SETTING_DECAY_TIME,
	SETTING_SUSTAIN_AMP,
	SETTING_RELEASE_TIME,
	SETTING_LFO_FREQ,
	SETTING_LFO_AMP,
	SETTING_INSTRUMENT_VOL,
	SETTING_FM_RATIO,
	SETTING_FM_AMP,
	SETTING_SQUARE_DUTY,
	SETTING_PITCH_BEND,
	SETTING_PITCH_SLIDE,
	SETTING_START_PHASE,
	SETTING_LFO_PHASE,
	SETTING_COUNT
};

enum FXSetting
{
	FXSETTING_LINEAR_FADE_START_AMP,
	FXSETTING_LINEAR_FADE_END_AMP,
	FXSETTING_EXP_FADE_START_AMP,
	FXSETTING_EXP_FADE_END_AMP,
	FXSETTING_AMP_AMP,
	FXSETTING_CLIP_AMP,
	FXSETTING_COUNT
};

enum FX
{
	FX_LINEAR_FADE,
	FX_EXP_FADE,
	FX_AMP,
	FX_CLIP,
	FX_COUNT
};

enum OP
{
	OP_ASSIGN,
	OP_ADD,
	OP_SUB,
	OP_MUL,
	OP_COUNT
};

define_map(map_u32_f, node_u32_f, uint32_t, float, 0);

typedef struct
{
	map_u32_f instr;
} Frame;

define_vector(vector_frame, Frame);

typedef struct
{
	uint32_t r[64];
	vector_f samples;
	vector_frame frames;
	uint64_t current_us;
	uint32_t ip;
	uint32_t sample_rate;
	uint32_t tick_length;
	uint16_t settings[SETTING_COUNT];
	uint16_t fxsettings[FXSETTING_COUNT];
	uint16_t fps;
} State;

static void state_init(State *state, uint32_t sample_rate, uint16_t fps, uint32_t time_max)
{
	state->sample_rate = sample_rate;
	state->samples     = vector_f_create((uint64_t)time_max * sample_rate);
	state->frames      = vector_frame_create((uint64_t)time_max * fps);
	state->fps         = fps;
	state->ip          = 0;
	state->current_us  = 0;
	state->tick_length = 1000000;
	static const uint16_t default_settings[SETTING_COUNT] = {
		0, 0, 65535, 0, 65535, 0, 0, 0, 256, 0, 0, 32768, 0, 0, 0, 0
	};
	static const uint16_t default_fxsettings[FXSETTING_COUNT] = {
		4096, 4096, 4096, 4096, 4096, 4096
	};
	memcpy(state->settings, default_settings, sizeof(uint16_t) * SETTING_COUNT);
	memcpy(state->fxsettings, default_fxsettings, sizeof(uint16_t) * FXSETTING_COUNT);
	memset(state->r, 0, sizeof(uint32_t) * 64);
}

static void state_destroy(State *state)
{
	vector_f_destroy(&state->samples);
	for (uint64_t i = 0; i < state->frames.length; i++)
		map_u32_f_destroy(&state->frames.data[i].instr);
	vector_frame_destroy(&state->frames);
}

static double osc(State *state, double t)
{
	switch (state->settings[SETTING_INSTRUMENT])
	{
	case INSTRUMENT_SINE:
		return sin(2.0 * M_PI * t);
	case INSTRUMENT_SQUARE:
	{
		double duty = (1.0 / 65536.0) * state->settings[SETTING_SQUARE_DUTY];
		double dc = -1.0 + 2.0 * duty;
		t -= floor(t);
		return t < duty ? dc - 1.0 : dc + 1.0;
	}
	case INSTRUMENT_TRIANGLE:
		t -= floor(t);
		if (t < 0.25) return t * 4.0;
		if (t < 0.75) return 2.0 - t * 4.0;
		return t * 4.0 - 4.0;
	case INSTRUMENT_SAWTOOTH:
		t -= floor(t);
		return t * 2.0 - 1.0;
	case INSTRUMENT_FM:
	{
		double ratio = (1.0 / 4096.0) * state->settings[SETTING_FM_RATIO];
		double amp = (1.0 / 4096.0) * state->settings[SETTING_FM_AMP];
		if (ratio != 0.0)
			t += amp / (2.0 * M_PI * ratio) * cos(2.0 * M_PI * ratio * t);
		return sin(2.0 * M_PI * t);
	}
	case INSTRUMENT_NOISE:
	{
		uint64_t x = t * 2.0;
		x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9;
		x = (x ^ (x >> 27)) * 0x94d049bb133111eb;
		x = x ^ (x >> 31);
		return (1.0f / (1ULL << 63)) * (double)(int64_t)x;
	}
	case INSTRUMENT_CUBIC:
	{
		static const double cubic_mul = 12.0 * sqrt(3.0);
		t -= floor(t);
		return cubic_mul * t * (t - 0.5f) * (t - 1.0f);
	}
	case INSTRUMENT_HILBERT_TRI:
	{
		int64_t it = floor(t * 2.0);
		t -= 0.5 * it;
		if (t > 0.25) t = 0.5 - t;                // G is catalan's constant
		static const double C1 = -0.368551627937; // 8/pi * (1 - ln pi)
		static const double C2 = 0.0825252536247; // 8/pi * (48G/pi + 8 ln pi - 16 ln 2 - 12.03)
		static const double C3 = -3.09337140887;  // 8/pi * (-128G/pi - 16 ln pi + 32 ln 2 + 32.24)
		static const double C4 = -1.22230996295;  // 8/pi * -12/25
		static const double M  = -2.54647908947;  // -8/pi
		double r = fma(t, C4, C3);
		r = fma(r, t, C2);
		r = fma(r, t, C1);
		double s = t ? M * t * log(t) : 0.0;
		r = fma(r, t, s);
		if (it % 2) r *= -1.0;
		return r;
	}
	case INSTRUMENT_NES_TRI:
	{
		static const double NES_TRI_SAMPLES[32] = {
			   1.0/15,   3.0/15,   5.0/15,  7.0/15,  9.0/15,  11.0/15,  13.0/15,  15.0/15,
			  15.0/15,  13.0/15,  11.0/15,  9.0/15,  7.0/15,   5.0/15,   3.0/15,   1.0/15,
			  -1.0/15,  -3.0/15,  -5.0/15, -7.0/15, -9.0/15, -11.0/15, -13.0/15, -15.0/15,
			 -15.0/15, -13.0/15, -11.0/15, -9.0/15, -7.0/15,  -5.0/15,  -3.0/15,  -1.0/15,
		};
		int64_t ti = t * 32.0;
		return NES_TRI_SAMPLES[ti & 31];
	}
	default:
		return 0.0;
	};
}

static IndexPair render_note(State *state, int8_t note, uint16_t ticks)
{
	double fnote = (double)note + (1.0 / 65536.0) * (int16_t)state->settings[SETTING_PITCH_BEND];
	double base_freq   = 440.0 * pow(2.0, fnote / 12.0);
	double start_phase = (1.0 / 65536.0) * state->settings[SETTING_START_PHASE];
	double lfo_omega = (M_PI / 128.0) * state->settings[SETTING_LFO_FREQ];
	double lfo_amp   = (1.0 / 65536.0) * state->settings[SETTING_LFO_AMP];
	double lfo_phase = (M_PI / 32768.0) * state->settings[SETTING_LFO_PHASE];
	double pitch_slide = (log(2.0) / 1200.0) * (int16_t)state->settings[SETTING_PITCH_SLIDE];
	float instr_amp = (1.0f / 256.0f) * state->settings[SETTING_INSTRUMENT_VOL];

	uint64_t start_time   = state->current_us;
	uint64_t sustain_time = start_time + state->tick_length * ticks;
	uint64_t attack_time  = start_time + 1000000ULL * state->settings[SETTING_ATTACK_TIME] / 256;
	uint64_t decay_time   = attack_time + 1000000ULL * state->settings[SETTING_DECAY_TIME] / 256;
	uint64_t release_time = sustain_time + 1000000ULL * state->settings[SETTING_RELEASE_TIME] / 256;

	uint64_t start_sample   =   start_time * state->sample_rate / 1000000;
	uint64_t sustain_sample = sustain_time * state->sample_rate / 1000000;
	uint64_t attack_sample  =  attack_time * state->sample_rate / 1000000;
	uint64_t decay_sample   =   decay_time * state->sample_rate / 1000000;
	uint64_t release_sample = release_time * state->sample_rate / 1000000;

	float adsr_amp = 0.0f;

	IndexPair range = vector_f_ensure(&state->samples, (IndexPair){ start_sample, release_sample });
	if (note == -128) return range;
	for (uint64_t i = range.begin; i < range.end; i++)
	{
		float a;
		if (i >= sustain_sample)
			a = adsr_amp - adsr_amp * (i - sustain_sample) / (release_sample - sustain_sample);
		else if (i >= decay_sample)
			a = adsr_amp = (1.0f / 65536.0f) * state->settings[SETTING_SUSTAIN_AMP];
		else if (i >= attack_sample)
		{
			float a2 = (1.0f / 65536.0f) * state->settings[SETTING_SUSTAIN_AMP];
			float a1 = (1.0f / 65536.0f) * state->settings[SETTING_ATTACK_AMP];
			a = adsr_amp = a1 + (a2 - a1) * (i - attack_sample) / (decay_sample - attack_sample);
		}
		else
		{
			float amp = (1.0f / 65536.0f) * state->settings[SETTING_ATTACK_AMP];
			a = adsr_amp = amp * (i - start_sample) / (attack_sample - start_sample);
		}
		double t = (double)(i - start_sample) / state->sample_rate;
		double p = pitch_slide == 0.0 ? t : expm1(pitch_slide * t) / pitch_slide;
		if (lfo_amp != 0.0)
		{
			double den = lfo_omega*lfo_omega + pitch_slide*pitch_slide;
			if (den == 0.0) p += t * lfo_amp * sin(lfo_phase);
			else
			{
				double wp = exp(pitch_slide * t) * cos(lfo_omega * t + lfo_phase) - cos(lfo_phase);
				double lp = exp(pitch_slide * t) * sin(lfo_omega * t + lfo_phase) - sin(lfo_phase);
				p += lfo_amp / den * (-lfo_omega * wp + pitch_slide * lp);
			}
		}
		state->samples.data[i] += a * instr_amp * osc(state, fma(base_freq, p, start_phase));
	}
	return range;
}

static void _fx(State *state, uint8_t fx, uint64_t a, uint64_t b, uint64_t begin, uint64_t end)
{
	switch (fx)
	{
	case FX_LINEAR_FADE:
	{
		float amp = (1.0f / 4096.0f) * state->fxsettings[FXSETTING_LINEAR_FADE_START_AMP];
		float amp2 = (1.0f / 4096.0f) * state->fxsettings[FXSETTING_LINEAR_FADE_END_AMP];
		float inc = (amp2 - amp) / (b - a);
		amp += (amp2 - amp) * (begin - a) / (b - a);
		for (uint64_t i = begin; i < end; i++)
		{
			state->samples.data[i] *= amp;
			amp += inc;
		}
		break;
	}
	case FX_EXP_FADE:
	{
		float amp = (1.0f / 4096.0f) * state->fxsettings[FXSETTING_EXP_FADE_START_AMP];
		float amp2 = (1.0f / 4096.0f) * state->fxsettings[FXSETTING_EXP_FADE_END_AMP];
		float mul = powf(amp2 / amp, 1.0f / (b - a));
		amp *= powf(amp2 / amp, (begin - a) / (b - a));
		for (uint64_t i = begin; i < end; i++)
		{
			state->samples.data[i] *= amp;
			amp *= mul;
		}
		break;
	}
	case FX_AMP:
	{
		float amp = (1.0f / 4096.0f) * state->fxsettings[FXSETTING_AMP_AMP];
		for (uint64_t i = begin; i < end; i++)
			state->samples.data[i] *= amp;
		break;
	}
	case FX_CLIP:
	{
		float amp = (1.0f / 4096.0f) * state->fxsettings[FXSETTING_CLIP_AMP];
		for (uint64_t i = begin; i < end; i++)
		{
			float s = state->samples.data[i];
			if (s > amp) s = amp;
			if (s < -amp) s = -amp;
			state->samples.data[i] = s;
		}
		break;
	}
	}
}

static IndexPair apply_fx(State *state, uint8_t fx, uint16_t ticks)
{
	uint64_t start_time = state->current_us;
	uint64_t end_time   = start_time + state->tick_length * ticks;
	uint64_t start_sample = start_time * state->sample_rate / 1000000;
	uint64_t end_sample   =   end_time * state->sample_rate / 1000000;
	IndexPair range = vector_f_ensure(&state->samples, (IndexPair){ start_sample, end_sample });
	_fx(state, fx, start_sample, end_sample, range.begin, range.end);
	return range;
}

enum StepError
{
	STEP_SUCCESS,
	STEP_EXIT,
	STEP_EOF,
	STEP_UNK,
	STEP_MAX
};

IndexPair mark_frames(State *state, IndexPair range, uint32_t key)
{
	if (range.begin < range.end)
	{
		uint64_t frame_begin = range.begin * state->fps / state->sample_rate;
		uint64_t frame_end = (range.end * state->fps + state->sample_rate - 1) / state->sample_rate;
		IndexPair f_range = vector_frame_ensure(&state->frames, (IndexPair){ frame_begin, frame_end });
		for (uint64_t i = f_range.begin; i < f_range.end; i++)
		{
			float *times = map_u32_f_get(&state->frames.data[i].instr, state->ip);
			if (times != NULL) *times += 1.0f;
		}
		return f_range;
	}
	return (IndexPair){ 0, 0 };
}

static enum StepError state_step(State *state, const uint8_t *prog, uint64_t prog_length)
{
	if (state->ip >= prog_length) return STEP_EOF;
	const uint8_t *instr = prog + state->ip * 4;
	switch (instr[0])
	{
	case 0:
		state->tick_length = get_u24(instr, 1);
		state->ip++;
		return STEP_SUCCESS;
	case 1:
	{
		int8_t note = get_s8(instr, 1);
		uint16_t ticks = get_u16(instr, 2);
		mark_frames(state, render_note(state, note, ticks), state->ip);
		state->current_us += state->tick_length * ticks;
		state->ip++;
		return STEP_SUCCESS;
		break;
	}
	case 2:
	{
		int8_t note = get_s8(instr, 1);
		uint16_t ticks = get_u16(instr, 2);
		mark_frames(state, render_note(state, note, ticks), state->ip);
		state->ip++;
		return STEP_SUCCESS;
	}
	case 3:
	{
		uint8_t setting = get_u8(instr, 1);
		if (setting < SETTING_COUNT)
			state->settings[setting] = get_u16(instr, 2);
		state->ip++;
		return STEP_SUCCESS;
	}
	case 4:
	{
		state->current_us += (int64_t)get_s24(instr, 1) * state->tick_length;
		if (state->current_us >= 0x8000000000000000ULL)
			state->current_us = 0;
		state->ip++;
		return STEP_SUCCESS;
	}
	case 5:
	{
		uint8_t fxsetting = get_u8(instr, 1);
		if (fxsetting < FXSETTING_COUNT)
			state->fxsettings[fxsetting] = get_u16(instr, 2);
		state->ip++;
		return STEP_SUCCESS;
	}
	case 6:
	{
		int8_t fx = get_s8(instr, 1);
		uint16_t ticks = get_u16(instr, 2);
		mark_frames(state, apply_fx(state, fx, ticks), state->ip);
		state->current_us += state->tick_length * ticks;
		state->ip++;
		return STEP_SUCCESS;
	}
	case 7:
	{
		int8_t fx = get_s8(instr, 1);
		uint16_t ticks = get_u16(instr, 2);
		mark_frames(state, apply_fx(state, fx, ticks), state->ip);
		state->ip++;
		return STEP_SUCCESS;
	}
	case 8:
	{
		uint8_t op = get_u8(instr, 1);
		uint8_t reg = op & 0x3F;
		op >>= 6;
		uint16_t val = get_s16(instr, 2);
		switch (op)
		{
		case 0: state->r[reg]  = val; break;
		case 1: state->r[reg] += val; break;
		case 2: state->r[reg] -= val; break;
		case 3: state->r[reg] *= val; break;
		}
		state->ip++;
		return STEP_SUCCESS;
	}
	case 9:
	{
		uint8_t cond = get_u8(instr, 1);
		uint8_t reg = cond & 0x3F;
		cond >>= 6;
		int16_t jmp = get_s16(instr, 2);
		uint8_t dojmp = 0;
		switch (cond)
		{
		case 0: dojmp = state->r[reg] == 0; break;
		case 1: dojmp = state->r[reg] != 0; break;
		case 2: dojmp = state->r[reg]  > 0; break;
		case 3: dojmp = state->r[reg]  < 0; break;
		}
		if (dojmp)
			state->ip += jmp;
		else state->ip++;
		return STEP_SUCCESS;
	}
	case 0xFF:
		return STEP_EXIT;
	default:
		return STEP_UNK;
	}
}

static uint8_t little_endian()
{
	volatile uint16_t n = 0x0001;
	return *(uint8_t *)&n;
}

static uint8_t write_wav(const State *state, const RenderInput *input, RenderStatus *status)
{
	#define write_num(file, type, val) \
	{ \
		for (uint64_t i = 0; i < sizeof(type) * 8; i += 8) \
			if (fputc((type)(val) >> i, file) == EOF) goto err; \
	}
	#define write_pun(file, type, type2, val) \
	{ \
		type tmp; \
		memcpy(&tmp, &val, sizeof(type)); \
		for (uint64_t i = 0; i < sizeof(type) * 8; i += 8) \
			if (fputc(tmp >> i, file) == EOF) goto err; \
	}
	#define write_str(file, val) \
		if (fwrite(val, 1, strlen(val), file) < strlen(val)) goto err;

	FILE *file = fopen(input->opt.audio_filename, "wb");
	if (file == NULL) return 1;
	write_str(file, "RIFF");
	long pos_riff_cksize = ftell(file);
	if (pos_riff_cksize == -1) goto err;
	write_num(file, uint32_t, 0);
	write_str(file, "WAVEfmt ");
	long pos_fmt_cksize = ftell(file);
	if (pos_fmt_cksize == -1) goto err;
	write_num(file, uint32_t, 0);
	write_num(file, uint16_t, 3);
	write_num(file, uint16_t, 1);
	write_num(file, uint32_t, state->sample_rate);
	write_num(file, uint32_t, state->sample_rate * sizeof(float));
	write_num(file, uint16_t, sizeof(float));
	write_num(file, uint16_t, 8 * sizeof(float));
	long pos_data_id = ftell(file);
	if (pos_data_id == -1) goto err;
	if (fseek(file, pos_fmt_cksize, SEEK_SET)) goto err;
	write_num(file, uint32_t, pos_data_id - pos_fmt_cksize - 4);
	if (fseek(file, pos_data_id, SEEK_SET)) goto err;
	write_str(file, "data");
	long pos_data_cksize = ftell(file);
	if (pos_data_cksize == -1) goto err;
	write_num(file, uint32_t, 0);
	for (uint64_t i = 0; i < state->samples.length; i++)
	{
		write_pun(file, uint32_t, float, state->samples.data[i]);
		status->samples_saved++;
	}
	long pos_end = ftell(file);
	if (pos_end == -1) goto err;
	if (fseek(file, pos_data_cksize, SEEK_SET)) goto err;
	write_num(file, uint32_t, pos_end - pos_data_cksize - 4);
	if (fseek(file, pos_riff_cksize, SEEK_SET)) goto err;
	write_num(file, uint32_t, pos_end - pos_riff_cksize - 4);
	fclose(file);
	return 0;
err:
	fclose(file);
	return RENDER_STATUS_ERR_AUDIO;
}

static int frame_filename_filter(const struct dirent *file)
{
	char suf[256];
	int n;
	if (sscanf(file->d_name, "frame%d%255s", &n, suf) < 2) return 0;
	if (strcmp(suf, ".ppm") != 0) return 0;
	return 1;
}

#define RFC_FREQ_BINS 2048

typedef struct
{
	float freq_real[RFC_FREQ_BINS];
	float freq_imag[RFC_FREQ_BINS];
	float freq_prev[RFC_FREQ_BINS];

	float twiddle[RFC_FREQ_BINS];
	float window[RFC_FREQ_BINS];
	float instr_bright[];
} RenderFrameContext;

void rfc_get_frequency(RenderFrameContext *rfc)
{
	float *ar = rfc->freq_real;
	float *ai = rfc->freq_imag;
	float *tw = rfc->twiddle;
	for (uint64_t i = 0; i < RFC_FREQ_BINS; i++)
		ai[i] = 0.0f;
	for (uint64_t i = 0; i < RFC_FREQ_BINS; i++)
		ar[i] *= rfc->window[i];
	for (uint64_t i = 0; i < RFC_FREQ_BINS; i++)
	{
		uint64_t j = 0;
		for (uint64_t k = 1, l = i; k < RFC_FREQ_BINS; k <<= 1, l >>= 1)
			j = (j << 1) | (l & 1);
		if (i >= j) continue;
		float tmp = ar[i];
		ar[i] = ar[j];
		ar[j] = tmp;
	}
	for (uint64_t s = 1; s < RFC_FREQ_BINS; s <<= 1)
		for (uint64_t i = 0; i < RFC_FREQ_BINS; i += (s << 1))
			for (uint64_t j = 0; j < s; j++)
			{
				float r1 = ar[i+j];
				float i1 = ai[i+j];
				float r2 = ar[i+j+s]*tw[j*RFC_FREQ_BINS/s] - ai[i+j+s]*tw[j*RFC_FREQ_BINS/s+1];
				float i2 = ai[i+j+s]*tw[j*RFC_FREQ_BINS/s] + ar[i+j+s]*tw[j*RFC_FREQ_BINS/s+1];
				ar[i+j] = r1 + r2;
				ai[i+j] = i1 + i2;
				ar[i+j+s] = r1 - r2;
				ai[i+j+s] = i1 - i2;
			}
	for (uint64_t i = 0; i < RFC_FREQ_BINS; i++)
	{
		float phase = atan2f(ai[i], ar[i]);
		ar[i] = (logf(ar[i]*ar[i] + ai[i]*ai[i] + (1.0f / 8388608.0f)) - 2.0f * logf(RFC_FREQ_BINS)) * (5.0f / logf(10.0f));
		ai[i] = phase;
	}
}

static RenderFrameContext *render_frame_start(const State *state, const RenderInput *input, uint8_t *img)
{
	RenderFrameContext *rfc = malloc(sizeof(RenderFrameContext) + sizeof(float) * input->length);
	if (rfc == NULL) return NULL;
	for (uint64_t i = 0; i < RFC_FREQ_BINS; i += 2)
	{
		rfc->twiddle[i]   = cosf(M_PI * i / RFC_FREQ_BINS);
		rfc->twiddle[i+1] = sinf(M_PI * i / RFC_FREQ_BINS);
	}
	for (uint64_t i = 0; i < RFC_FREQ_BINS; i++)
	{
		float s = sinf(M_PI * i / RFC_FREQ_BINS);
		rfc->window[i] = s*s;
	}
	for (uint64_t i = 0; i < RFC_FREQ_BINS; i++)
		rfc->freq_prev[i] = -80.0f;
	for (uint64_t i = 0; i < input->length; i++)
		rfc->instr_bright[i] = 0.0f;
	return rfc;
}

static void render_frame(RenderFrameContext *rfc, const State *state, const RenderInput *input, uint8_t *img, uint32_t t)
{
	const uint16_t W = input->opt.width;
	const uint16_t H = input->opt.height;
	uint16_t split_x1 = 1;
	uint16_t split_x2 = W >> 2;
	uint16_t split_x3 = W - 1;
	uint16_t split_y1 = 1;
	uint16_t split_y2 = H >> 2;
	uint16_t split_y3 = H - 1;
	map_u32_f *executing_instrs = &state->frames.data[t].instr;

	memset(img, 32, 3*W*H);

	// piano
	for (uint16_t y = split_y1; y < split_y2; y++)
		for (uint16_t x = split_x2; x < split_x3; x++)
		{
			img[(y*W+x)*3] = 255;
			img[(y*W+x)*3+1] = 255;
			img[(y*W+x)*3+2] = 255;
		}
	const uint16_t octaves = 8;
	const uint16_t black_note_h = (uint32_t)(split_y2 - split_y1) * 2 / 3 + split_y1;
	for (uint16_t i = 0; i < octaves * 7; i++)
	{
		uint16_t x = (uint32_t)(split_x3 - split_x2) * i / (octaves*7) + split_x2;
		for (uint16_t y = split_y1; y < split_y2; y++)
		{
			img[(y*W+x)*3] = 0;
			img[(y*W+x)*3+1] = 0;
			img[(y*W+x)*3+2] = 0;
		}
		uint16_t a = i % 7;
		if (a == 0 || a == 3) continue;
		uint16_t black_note_w = (split_x3 - split_x2) / (octaves*28);
		uint16_t x1 = x - black_note_w + 1;
		uint16_t x2 = x + black_note_w;
		for (uint16_t y = split_y1; y < black_note_h; y++)
			for (uint16_t x = x1; x < x2; x++)
			{
				img[(y*W+x)*3] = 0;
				img[(y*W+x)*3+1] = 0;
				img[(y*W+x)*3+2] = 0;
			}
	}

	// instruction list
	for (uint16_t i = split_y2; i < split_y3; i++)
	{
		uint64_t ip = (uint64_t)input->length * (i - split_y2) / (split_y3 - split_y2);
		for (uint8_t j = 0; j < 4; j++)
		{
			uint16_t x1 = (uint32_t)(split_x2 - split_x1) *    j  / 4 + split_x1;
			uint16_t x2 = (uint32_t)(split_x2 - split_x1) * (j+1) / 4 + split_x1;
			uint8_t col = 0;
			for (int i = 0; i < 8; i++)
				col = (col << 1) | ((input->instr[ip*4+j] >> i) & 1);
			for (uint16_t x = x1; x < x2; x++)
			{
				img[(i*W+x)*3] = col;
				img[(i*W+x)*3+1] = col;
				img[(i*W+x)*3+2] = col;
			}
		}
	}

	// visualizer
	const float MIN_FREQ = 20.0f;
	const float MAX_FREQ = 8000.0f;
	const uint64_t MIN_FREQ_BIN = ceilf((float)RFC_FREQ_BINS * MIN_FREQ / input->opt.sample_rate);
	const uint64_t MAX_FREQ_BIN = floorf((float)RFC_FREQ_BINS * MAX_FREQ / input->opt.sample_rate);
	int64_t samples_end = (int64_t)(t + 1) * input->opt.sample_rate / input->opt.fps;
	int64_t samples_begin = samples_end - RFC_FREQ_BINS;
	int64_t endpoint_2 = samples_end;
	if (endpoint_2 > state->samples.length) endpoint_2 = state->samples.length;
	int64_t endpoint_1 = endpoint_2;
	if (endpoint_1 > 0) endpoint_1 = 0;
	int64_t idx = samples_begin;
	for (; idx < endpoint_1; idx++)
		rfc->freq_real[idx - samples_begin] = 0.0f;
	for (; idx < endpoint_2; idx++)
		rfc->freq_real[idx - samples_begin] = state->samples.data[idx];
	for (; idx < samples_end; idx++)
		rfc->freq_real[idx - samples_begin] = 0.0f;
	rfc_get_frequency(rfc);
	for (uint64_t i = 0; i < RFC_FREQ_BINS; i++)
		rfc->freq_prev[i] = fmaxf(rfc->freq_prev[i] - (30.0f / input->opt.fps), rfc->freq_real[i]);
	for (uint16_t x = split_x2; x < split_x3; x++)
	{
		float s = 0.0f;
		float x1 = (float)(x   - split_x2) * (MAX_FREQ_BIN - MIN_FREQ_BIN) / (split_x3 - split_x2 + 2) + MIN_FREQ_BIN;
		float x2 = (float)(x+3 - split_x2) * (MAX_FREQ_BIN - MIN_FREQ_BIN) / (split_x3 - split_x2 + 2) + MIN_FREQ_BIN;
		uint64_t rx1 = floorf(x1);
		uint64_t rx2 = ceilf(x2);
		for (uint64_t i = rx1; i < rx2; i++)
			s += rfc->freq_prev[i] * (1.0f - fabsf(fmaxf(x1, fminf(x2, i)) - i));
		s /= x2 - x1;
		float h = (float)split_y3 - (s + 60.0f) / 66.0f * (split_y3 - split_y2);
		for (uint16_t y = h; y < split_y3; y++)
			img[(y*W+x)*3] = 255;
	}

	// waveform
	uint64_t max_mag_freq = 0;
	float max_mag = -696.9f;
	float max_mag_angle = 0.0f;
	for (uint64_t i = MIN_FREQ_BIN; i <= MAX_FREQ_BIN; i++)
		if (max_mag < rfc->freq_real[i])
		{
			max_mag_freq = i;
			max_mag = rfc->freq_real[i];
			max_mag_angle = rfc->freq_imag[i];
		}
	int64_t offset = 0;
	if (max_mag_freq != 0) offset = max_mag_angle * RFC_FREQ_BINS / (2.0f*M_PI*max_mag_freq);
	samples_end = (int64_t)(t + 1) * input->opt.sample_rate / input->opt.fps + offset;
	samples_begin = samples_end - RFC_FREQ_BINS;
	endpoint_2 = samples_end;
	if (endpoint_2 > state->samples.length) endpoint_2 = state->samples.length;
	endpoint_1 = endpoint_2;
	if (endpoint_1 > 0) endpoint_1 = 0;
	idx = samples_begin;
	for (; idx < endpoint_1; idx++)
		rfc->freq_real[idx - samples_begin] = 0.0f;
	for (; idx < endpoint_2; idx++)
		rfc->freq_real[idx - samples_begin] = state->samples.data[idx];
	for (; idx < samples_end; idx++)
		rfc->freq_real[idx - samples_begin] = 0.0f;
	int prev_h = split_y1 + (split_y2 - split_y1) / 2;
	for (uint16_t x = split_x1; x < split_x2; x++)
	{
		float s = 0.0f;
		float x1 = (float)(x   - split_x1) * RFC_FREQ_BINS / (split_x2 - split_x1 + 2);
		float x2 = (float)(x+3 - split_x1) * RFC_FREQ_BINS / (split_x2 - split_x1 + 2);
		uint64_t rx1 = floorf(x1);
		uint64_t rx2 = ceilf(x2);
		for (uint64_t i = rx1; i < rx2; i++)
			s += rfc->freq_real[i] * (1.0f - fabsf(fmaxf(x1, fminf(x2, i)) - i));
		s /= x2 - x1;
		int h1 = fmaxf(split_y1, fminf(split_y2-1, (float)split_y1 + (float)(split_y2 - split_y1) * (0.5f - s * 0.5f)));
		int h2 = prev_h;
		prev_h = h1;
		if (h1 > h2)
		{
			int t = h1;
			h1 = h2;
			h2 = t;
		}
		for (uint16_t y = h1; y <= h2; y++)
		{
			img[(y*W+x)*3] = 255;
			img[(y*W+x)*3+1] = 255;
			img[(y*W+x)*3+2] = 255;
		}
	}

	// highlight (instructions)
	for (uint32_t i = 0; i < input->length; i++)
	{
		rfc->instr_bright[i] -= 12.0f / input->opt.fps;
		if (rfc->instr_bright[i] < 0.0f) rfc->instr_bright[i] = 0.0f;
	}
	for (node_u32_f *it = map_u32_f_begin(executing_instrs); it; it = map_u32_f_next(it))
		rfc->instr_bright[it->key] = log2(pow(2.0f, rfc->instr_bright[it->key]) + it->value);
	float rect_dilate = fminf(W, H) * (1.0f / 80.0f);
	for (uint32_t i = 0; i < input->length; i++)
		if (rfc->instr_bright[i] > 0.125f / input->opt.fps)
		{
			float fx1 = (float)split_x1;
			float fx2 = (float)split_x2;
			int x1 = fmaxf(0, floorf(fx1 - rect_dilate));
			int x2 = fminf(W, ceilf(fx2 + rect_dilate));
			float fy1 = (float)(split_y3 - split_y2) *    i  / input->length + split_y2;
			float fy2 = (float)(split_y3 - split_y2) * (i+1) / input->length + split_y2;
			int y1 = fmaxf(0, floorf(fy1 - rect_dilate));
			int y2 = fminf(H, ceilf(fy2 + rect_dilate));
			int cmd = get_u8(input->instr + i*4, 0);
			int is_note = cmd == 1 || cmd == 2;
			float colr = 208.0f;
			float colg = is_note ? 208 : 60;
			float colb = is_note ? 60 : 208;
			for (int y = y1; y < y2; y++)
				for (int x = x1; x < x2; x++)
				{
					float xc = fmaxf(fx1, fminf(fx2, x));
					float yc = fmaxf(fy1, fminf(fy2, y));
					float d = hypotf(xc - x, yc - y);
					float l = rfc->instr_bright[i] * fmaxf(0.0f, 1.0f - d / rect_dilate);
					img[(y*W+x)*3] = fminf(255, colr * l + img[(y*W+x)*3]);
					img[(y*W+x)*3+1] = fminf(255, colg * l + img[(y*W+x)*3+1]);
					img[(y*W+x)*3+2] = fminf(255, colb * l + img[(y*W+x)*3+2]);
				}
		}
	// highlight (piano)
	const int8_t lowest_C = -9 - 12*4;
	const float highlight_radius = (float)(split_x3 - split_x2) / (octaves*10);
	for (node_u32_f *it = map_u32_f_begin(executing_instrs); it; it = map_u32_f_next(it))
	{
		int cmd = get_u8(input->instr + it->key*4, 0);
		if (cmd != 1 && cmd != 2) continue;
		int8_t note = get_s8(input->instr + it->key*4, 1);
		if (note < lowest_C || note >= lowest_C + octaves*12) continue;
		int8_t note_rel = note - lowest_C;
		int8_t octave = note_rel / 12;
		int8_t key = note_rel % 12;
		const int8_t xpos2[12] = { 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13 };
		const int8_t black[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
		float xc = ((float)octave + (float)xpos2[key] / 14.0f) * (split_x3 - split_x2) / octaves + split_x2;
		float yc = black[key] ? black_note_h : split_y2;
		uint16_t x1 = fmaxf(0, floorf(xc - highlight_radius));
		uint16_t x2 = fminf(W, ceilf(xc + highlight_radius));
		uint16_t y1 = fmaxf(0, floorf(yc - highlight_radius));
		uint16_t y2 = fminf(H, ceilf(yc + highlight_radius));
		float colr = 208.0f;
		float colg = 60.0f;
		float colb = 60.0f;
		for (uint16_t y = y1; y < y2; y++)
			for (uint16_t x = x1; x < x2; x++)
			{
				float d = hypot(xc - x, yc - y);
				float alpha = fmaxf(0.0f, 1.0f - d / highlight_radius);
				img[(y*W+x)*3] = (1.0f - alpha) * img[(y*W+x)*3] + alpha * colr;
				img[(y*W+x)*3+1] = (1.0f - alpha) * img[(y*W+x)*3+1] + alpha * colg;
				img[(y*W+x)*3+2] = (1.0f - alpha) * img[(y*W+x)*3+2] + alpha * colb;
			}
	}
}

static void render_frame_end(RenderFrameContext *rfc)
{
	free(rfc);
}

static uint8_t write_ppms(const State *state, const RenderInput *input, RenderStatus *status)
{
	if (mkdir(input->opt.frames_dir, 0777) && errno != EEXIST) return 1;
	uint8_t err = 0;
	struct dirent **file_exist;
	int file_exist_length = scandir(input->opt.frames_dir, &file_exist, frame_filename_filter, alphasort);
	char filename[MAX_ARGS_LENGTH + 32];
	if (file_exist_length == -1) err |= RENDER_STATUS_ERR_VIDEO;
	else for (int i = 0; i < file_exist_length; i++)
	{
		sprintf(filename, "%s/%.30s", input->opt.frames_dir, file_exist[i]->d_name);
		if (remove(filename)) err |= RENDER_STATUS_ERR_VIDEO;
		free(file_exist[i]);
	}
	free(file_exist);
	file_exist = NULL;
	uint8_t *img = malloc(3 * input->opt.width * input->opt.height);
	if (img == NULL) return 1;
	RenderFrameContext *rfc = render_frame_start(state, input, img);
	char ppm_header[20];
	int ppm_header_size = sprintf(ppm_header, "P6\n%" PRIu16 " %" PRIu16 "\n255\n", input->opt.width, input->opt.height);
	for (uint32_t i = 0; i < state->frames.length; i++)
	{
		render_frame(rfc, state, input, img, i);
		sprintf(filename, "%s/frame%d.ppm", input->opt.frames_dir, i);
		FILE *file = fopen(filename, "wb");
		if (file == NULL)
		{
			err |= RENDER_STATUS_ERR_VIDEO;
			continue;
		}
		if (fwrite(ppm_header, 1, ppm_header_size, file) < ppm_header_size)
		{
			err |= RENDER_STATUS_ERR_VIDEO;
			fclose(file);
			continue;
		}
		if (fwrite(img, 1, 3 * input->opt.width * input->opt.height, file) < 3 * input->opt.width * input->opt.height)
			err |= RENDER_STATUS_ERR_VIDEO;
		else status->frames_saved++;
		fclose(file);
		if (input->ctx->terminate != 0)
		{
			err |= RENDER_STATUS_ERR_EARLY;
			break;
		}
	}
	free(img);
	render_frame_end(rfc);
	return err;
}

typedef struct
{
	const volatile _Atomic uint8_t *terminate;
	_Atomic uint64_t *sent;
	uint64_t div;
	uint64_t size;
	uint64_t chunk;
	void *buf;
	int fd;
	uint8_t reuse;
} SenderArgs;

static void *sender(void *arg)
{
	SenderArgs send = *(SenderArgs *)arg;
	uint64_t sent = 0;
	while (sent < send.size)
	{
		uint64_t send_bs = send.size - sent;
		if (send_bs > send.chunk) send_bs = send.chunk;
		ssize_t real_sent = write(send.fd, (char *)send.buf + sent, send_bs);
		if (real_sent == -1)
		{
			if (send.reuse == 0) close(send.fd);
			return (void *)1;
		}
		sent += real_sent;
		*(send.sent) = sent / send.div;
		if (*send.terminate) break;
	}
	if (send.reuse == 0) close(send.fd);
	return NULL;
}

static uint32_t write_ffmpeg(const State *state, const RenderInput *input, RenderStatus *status)
{
	if (!input->opt.generate_audio && !input->opt.generate_video) return 0;
	if (!input->opt.generate_video)
	{
		int pipefd[2];
		if (pipe(pipefd)) return RENDER_STATUS_ERR_AUDIO;
		char s[MAX_ARGS_LENGTH*2 + 256];
		sprintf(s, "ffmpeg -y -loglevel quiet -f f32%s -ar %d -ac 1 -i pipe:%d %s %s",
			little_endian() ? "le" : "be",
			input->opt.sample_rate,
			pipefd[0],
			input->opt.ffmpeg_args_audio,
			input->opt.audio_filename);
		wordexp_t ffmpeg_args;
		if (wordexp(s, &ffmpeg_args, 0))
		{
			close(pipefd[0]);
			close(pipefd[1]);
			return RENDER_STATUS_ERR_AUDIO;
		}
		pid_t pid = fork();
		if (pid == -1)
		{
			wordfree(&ffmpeg_args);
			close(pipefd[0]);
			close(pipefd[1]);
			return RENDER_STATUS_ERR_AUDIO;
		}
		if (pid == 0)
		{
			close(pipefd[1]);
			close(STDIN_FILENO);
			close(STDOUT_FILENO);
			close(STDERR_FILENO);
			execv(input->opt.ffmpeg_bin, ffmpeg_args.we_wordv);
			_exit(EXIT_FAILURE);
		}
		close(pipefd[0]);
		wordfree(&ffmpeg_args);
		SenderArgs send = {
			&input->ctx->terminate,
			&status->samples_saved,
			sizeof(float),
			sizeof(float) * state->samples.length,
			8192,
			state->samples.data,
			pipefd[1],
			0
		};
		uint8_t err = sender(&send) ? RENDER_STATUS_ERR_AUDIO : 0;
		if (input->ctx->terminate != 0) err |= RENDER_STATUS_ERR_EARLY;
		int wstatus;
		waitpid(pid, &wstatus, 0);
		if (!WIFEXITED(wstatus)) return err | RENDER_STATUS_ERR_AUDIO;
		return err;
	}
	int pipe_a[2], pipe_v[2];
	if (pipe(pipe_a)) return RENDER_STATUS_ERR_AUDIO | RENDER_STATUS_ERR_VIDEO;
	if (pipe(pipe_v))
	{
		close(pipe_a[0]);
		close(pipe_a[1]);
		return RENDER_STATUS_ERR_AUDIO | RENDER_STATUS_ERR_VIDEO;
	}
	char s[MAX_ARGS_LENGTH*4 + 256];
	int s_len = sprintf(s, "ffmpeg -y -loglevel quiet -f f32%s -ar %" PRIu32 " -ac 1 -i pipe:%d -f rawvideo -pixel_format rgb24 -video_size %" PRIu16 "x%" PRIu16 " -framerate %" PRIu16 " -i pipe:%d -map 0:a -map 1:v %s %s",
		little_endian() ? "le" : "be",
		input->opt.sample_rate,
		pipe_a[0],
		input->opt.width,
		input->opt.height,
		input->opt.fps,
		pipe_v[0],
		input->opt.ffmpeg_args_video,
		input->opt.video_filename);
	if (input->opt.generate_audio)
		sprintf(s + s_len, " -map 0:a %s %s", input->opt.ffmpeg_args_audio, input->opt.audio_filename);
	wordexp_t ffmpeg_args;
	if (wordexp(s, &ffmpeg_args, 0))
	{
		close(pipe_a[0]);
		close(pipe_a[1]);
		close(pipe_v[0]);
		close(pipe_v[1]);
		return RENDER_STATUS_ERR_AUDIO | RENDER_STATUS_ERR_VIDEO;
	}
	pid_t pid = fork();
	if (pid == -1)
	{
		wordfree(&ffmpeg_args);
		close(pipe_a[0]);
		close(pipe_a[1]);
		close(pipe_v[0]);
		close(pipe_v[1]);
		return RENDER_STATUS_ERR_AUDIO | RENDER_STATUS_ERR_VIDEO;
	}
	if (pid == 0)
	{
		close(pipe_a[1]);
		close(pipe_v[1]);
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		execv(input->opt.ffmpeg_bin, ffmpeg_args.we_wordv);
		_exit(EXIT_FAILURE);
	}
	close(pipe_a[0]);
	close(pipe_v[0]);
	wordfree(&ffmpeg_args);
	uint8_t err = 0;
	SenderArgs send_a = {
		&input->ctx->terminate,
		&status->samples_saved,
		sizeof(float),
		sizeof(float) * state->samples.length,
		8192,
		state->samples.data,
		pipe_a[1],
		0
	};
	pthread_t thread_a;
	int thread_err = pthread_create(&thread_a, NULL, sender, &send_a);
	uint8_t *img = malloc(3 * input->opt.width * input->opt.height);
	RenderFrameContext *rfc = render_frame_start(state, input, img);
	if (thread_err)
	{
		err |= RENDER_STATUS_ERR_VIDEO;
		close(pipe_a[1]);
	}
	if (img == NULL || rfc == NULL)
	{
		err |= RENDER_STATUS_ERR_VIDEO;
		free(img);
		render_frame_end(rfc);
		img = NULL;
		rfc = NULL;
		close(pipe_v[1]);
		if (!thread_err)
		{
			void *r;
			pthread_join(thread_a, &r);
			if (r) err |= RENDER_STATUS_ERR_AUDIO;
		}
		int wstatus;
		waitpid(pid, &wstatus, 0);
		return err;
	}
	_Atomic uint64_t trash;
	SenderArgs send_v = {
		&input->ctx->terminate,
		&trash,
		1,
		3 * input->opt.width * input->opt.height,
		8192,
		img,
		pipe_v[1],
		1
	};
	for (uint32_t i = 0; i < state->frames.length; i++)
	{
		render_frame(rfc, state, input, img, i);
		if (sender(&send_v)) err |= RENDER_STATUS_ERR_VIDEO;
		status->frames_saved++;
		if (input->ctx->terminate != 0) break;
	}
	close(pipe_v[1]);
	free(img);
	render_frame_end(rfc);
	if (!thread_err)
	{
		void *r;
		pthread_join(thread_a, &r);
		if (r) err |= RENDER_STATUS_ERR_AUDIO;
	}
	if (input->ctx->terminate != 0) err |= RENDER_STATUS_ERR_EARLY;
	int wstatus;
	waitpid(pid, &wstatus, 0);
	if (!WIFEXITED(wstatus)) return err | RENDER_STATUS_ERR_AUDIO | RENDER_STATUS_ERR_VIDEO;
	return err;
}

static uint8_t output(const State *state, const RenderInput *input, RenderStatus *status)
{
	uint8_t err = 0;
	if (!input->opt.use_ffmpeg)
	{
		if (input->opt.generate_audio)
			err |= write_wav(state, input, status);
		if (input->opt.generate_video)
			err |= write_ppms(state, input, status);
		return err;
	}
	return write_ffmpeg(state, input, status);
}

void render(RenderInput *input)
{
	State state;
	state_init(&state, input->opt.sample_rate, input->opt.fps, input->opt.time_max);
	memset(&input->ctx->status, 0, sizeof(RenderStatus));

	enum StepError step_err = STEP_SUCCESS;
	uint32_t steps = 0;
	while (input->ctx->terminate == 0)
	{
		if (steps >= input->opt.instructions_max)
		{
			step_err = STEP_MAX;
			break;
		}
		step_err = state_step(&state, input->instr, input->length);
		input->ctx->status.samples_estimate = state.samples.length;
		input->ctx->status.frames_estimate = state.frames.length;
		steps++;
		if (step_err == STEP_SUCCESS) continue;
		if (step_err == STEP_EXIT)
		{
			step_err = STEP_SUCCESS;
			break;
		}
		break;
	}

	switch (step_err)
	{
	case STEP_EOF: input->ctx->status.error |= RENDER_STATUS_ERR_EOF; break;
	case STEP_UNK: input->ctx->status.error |= RENDER_STATUS_ERR_UNK; break;
	case STEP_MAX: input->ctx->status.error |= RENDER_STATUS_ERR_MAX; break;
	case STEP_SUCCESS: break;
	default: break;
	}
	if (input->ctx->terminate == 1)
		input->ctx->status.error |= RENDER_STATUS_ERR_EARLY;
	else input->ctx->status.error |= output(&state, input, &input->ctx->status);
	state_destroy(&state);
	input->ctx->wait = 1;
	input->ctx->status.done = 1;
	while (input->ctx->terminate == 0)
		usleep(50000);
	free(input->instr);
	free(input->ctx);
	free(input);
}
