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
#include "simple_vector.h"
#include "vector.h"
#include "map.h"
#include "helper.h"

static double my_fma(double x, double y, double z)
{
#ifdef FP_FAST_FMA
	return fma(x, y, z);
#else
	return x * y + z;
#endif
}

define_vector(vector_u64, uint64_t, 0)
define_vector(vector_f, float, 0)

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
	INSTRUMENT_FILT_NOISE,
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
	SETTING_FILT_NOISE_FLOOR,
	SETTING_SAVE_PHASE,
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
	FXSETTING_BQ_START_NOTE,
	FXSETTING_BQ_END_NOTE,
	FXSETTING_BQ_STEP,
	FXSETTING_BQ_Q,
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

enum StepError
{
	STEP_SUCCESS,
	STEP_EXIT,
	STEP_EOF,
	STEP_UNK,
	STEP_MAX
};

typedef union RGB
{
	struct { float r, g, b; };
	float c[3];
} RGB;

typedef struct FrameInstr
{
	uint64_t ip;
	RGB col;
	int8_t note;
} FrameInstr;

define_simple_vector(simple_vector_fi, FrameInstr)

typedef struct
{
	simple_vector_fi instr;
} Frame;

define_vector(vector_frame, Frame, (Frame){simple_vector_fi_create(128)})
define_map(map_u32_u64, node_u32_u64, uint32_t, uint64_t, ~(uint64_t)0)

#define CALL_STACK_MAX 32
#define TRACK_STACK_MAX 32
#define SAVE_STACK_MAX 32

typedef struct
{
	int32_t r[64];
	int64_t current_us;
	double phases[5], lfo_phis[5];
	uint32_t tick_length;
	uint16_t settings[SETTING_COUNT];
	uint16_t fxsettings[FXSETTING_COUNT];
} Save;

typedef struct
{
	uint64_t call_stack[CALL_STACK_MAX];
	vector_f sample_stack[TRACK_STACK_MAX];
	vector_frame frame_stack[TRACK_STACK_MAX];
	map_u32_u64 label_cache;
	Save save_stack[SAVE_STACK_MAX];
	uint64_t label_saved;
	uint32_t ip;
	uint32_t sample_rate;
	uint32_t call_stack_ptr, track_stack_ptr, save_stack_ptr;
	uint16_t fps;
} State;

static void state_init(State *state, uint32_t sample_rate, uint16_t fps, uint32_t preroll_max, uint32_t time_max)
{
	state->sample_rate     = sample_rate;
	state->label_cache     = map_u32_u64_create();
	state->fps             = fps;
	state->label_saved     = 0;
	state->ip              = 0;
	state->call_stack_ptr  = 0;
	state->track_stack_ptr = 0;
	state->save_stack_ptr  = 0;
	for (int i = 0; i < TRACK_STACK_MAX; i++)
	{
		state->sample_stack[i] = vector_f_create(-(int64_t)preroll_max * sample_rate, (int64_t)time_max * sample_rate);
		state->frame_stack[i]  = vector_frame_create(-(int64_t)preroll_max * fps, (int64_t)time_max * fps);
	}
	static const uint16_t default_settings[SETTING_COUNT] = {
		0, 0, 65535, 0, 65535, 0, 0, 0, 256, 0, 0, 32768, 0, 0, 0, 0, 0, 0
	};
	static const uint16_t default_fxsettings[FXSETTING_COUNT] = {
		4096, 4096, 4096, 4096, 4096, 4096, 0, 0, 10, 0
	};
	Save *save = state->save_stack;
	save->current_us  = 0;
	save->tick_length = 1000000;
	memcpy(save->settings, default_settings, sizeof(uint16_t) * SETTING_COUNT);
	memcpy(save->fxsettings, default_fxsettings, sizeof(uint16_t) * FXSETTING_COUNT);
	memset(save->r, 0, sizeof save->r);
	memset(save->phases, 0, sizeof save->phases);
	memset(save->lfo_phis, 0, sizeof save->lfo_phis);
	memset(state->call_stack, 0, sizeof state->call_stack);
}

static void state_destroy(State *state)
{
	for (uint32_t i = 0; i <= state->track_stack_ptr; i++)
	{
		vector_f_destroy(state->sample_stack + i);
		for (int64_t j = state->frame_stack[i].begin; j < state->frame_stack[i].end; j++)
			simple_vector_fi_destroy(&vector_at(state->frame_stack + i, j).instr);
		vector_frame_destroy(state->frame_stack + i);
	}
	map_u32_u64_destroy(&state->label_cache);
}

static inline vector_f *curr_samples(State *state)
{
	return state->sample_stack + state->track_stack_ptr;
}

static inline const vector_f *curr_samples_c(const State *state)
{
	return state->sample_stack + state->track_stack_ptr;
}

static inline vector_frame *curr_frames(State *state)
{
	return state->frame_stack + state->track_stack_ptr;
}

static inline const vector_frame *curr_frames_c(const State *state)
{
	return state->frame_stack + state->track_stack_ptr;
}

static inline Save *curr_save(State *state)
{
	return state->save_stack + state->save_stack_ptr;
}

static double hash(double t)
{
	uint64_t x = t;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9;
	x = (x ^ (x >> 27)) * 0x94d049bb133111eb;
	x = x ^ (x >> 31);
	return (1.0f / (1ULL << 63)) * (double)(int64_t)x;
}

static double conv_kernel(double x, int r)
{
	if (x >= r) return 0.0;
	if (x <= -r) return 0.0;
	if (x >= -6.0e-9 && x <= 6.0e-9) return 1.0;
	double p1 = M_PI * x, p1r = p1 / r, p2r = p1r + p1r, p3r = p2r + p1r;
	double sinc = sin(p1) / p1;
	double window = 0.355768 + 0.487396 * cos(p1r) + 0.144232 * cos(p2r) + 0.012604 * cos(p3r);
	return sinc * window;
}

static double osc(State *state, double t)
{
	Save *save = curr_save(state);
	switch (save->settings[SETTING_INSTRUMENT])
	{
	case INSTRUMENT_SINE:
		return sin(2.0 * M_PI * t);
	case INSTRUMENT_SQUARE:
	{
		double duty = (1.0 / 65536.0) * save->settings[SETTING_SQUARE_DUTY];
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
		double ratio = (1.0 / 4096.0) * save->settings[SETTING_FM_RATIO];
		double amp = (1.0 / 4096.0) * save->settings[SETTING_FM_AMP];
		if (ratio != 0.0)
			t += amp / (2.0 * M_PI * ratio) * cos(2.0 * M_PI * ratio * t);
		return sin(2.0 * M_PI * t);
	}
	case INSTRUMENT_NOISE:
		return hash(floor(t));
	case INSTRUMENT_CUBIC:
	{
		static const double cubic_mul = 20.784609690826527522329356098070468403313; // 12√3
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
		double r = my_fma(t, C4, C3);
		r = my_fma(r, t, C2);
		r = my_fma(r, t, C1);
		double s = t ? M * t * log(t) : 0.0;
		r = my_fma(r, t, s);
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
	case INSTRUMENT_FILT_NOISE:
	{
		double t2 = t + t;
		static const int CONV_RADIUS = 64;
		double lp1 = 0.0;
		for (int i = -CONV_RADIUS; i <= CONV_RADIUS; i++)
		{
			double n = floor(t2) + i;
			lp1 += hash(n) * conv_kernel(n - t2, CONV_RADIUS);
		}
		double cut = (1.0 / 65536.0) * save->settings[SETTING_FILT_NOISE_FLOOR];
		if (cut == 0.0) return lp1;
		int new_r = ceil((double)CONV_RADIUS / cut);
		double lp2 = 0.0;
		for (int i = -new_r; i <= new_r; i++)
		{
			double n = floor(t2) + i;
			lp2 += hash(n) * conv_kernel((n - t2) * cut, CONV_RADIUS);
		}
		return lp1 - cut * lp2;
	}
	default:
		return 0.0;
	};
}

// ∫₀ᵗexp(pitch_slide ⬝ x)(1 + lfo_amp * sin(lfo_omega ⬝ x + lfo_phi))dx
static double phase_integral(double t, double pitch_slide, double lfo_amp, double lfo_omega, double lfo_phi)
{
	double p = pitch_slide == 0.0 ? t : expm1(pitch_slide * t) / pitch_slide;
	if (lfo_amp != 0.0)
	{
		double den = lfo_omega*lfo_omega + pitch_slide*pitch_slide;
		if (den == 0.0) p += t * lfo_amp * sin(lfo_phi);
		else
		{
			double wp = exp(pitch_slide * t) * cos(lfo_omega * t + lfo_phi) - cos(lfo_phi);
			double lp = exp(pitch_slide * t) * sin(lfo_omega * t + lfo_phi) - sin(lfo_phi);
			p += lfo_amp / den * (-lfo_omega * wp + pitch_slide * lp);
		}
	}
	return p;
}

static IndexPair render_note(State *state, int8_t note, uint64_t ticks)
{
	Save *save = curr_save(state);
	double fnote = (double)note + (1.0 / 65536.0) * (int16_t)save->settings[SETTING_PITCH_BEND];
	double base_freq   = 440.0 * pow(2.0, fnote / 12.0);
	double start_phase = (1.0 / 65536.0) * save->settings[SETTING_START_PHASE];
	double lfo_omega     = (M_PI / 128.0) * save->settings[SETTING_LFO_FREQ];
	double lfo_amp       = (1.0 / 65536.0) * save->settings[SETTING_LFO_AMP];
	double start_lfo_phi = (M_PI / 32768.0) * save->settings[SETTING_LFO_PHASE];
	double pitch_slide = (log(2.0) / 1200.0) * (int16_t)save->settings[SETTING_PITCH_SLIDE];
	float instr_amp = (1.0f / 256.0f) * save->settings[SETTING_INSTRUMENT_VOL];
	double saved_phase = 0.0, saved_lfo_phi = 0.0;
	if (save->settings[SETTING_SAVE_PHASE] != 0)
	{
		uint16_t val = save->settings[SETTING_SAVE_PHASE];
		saved_lfo_phi = save->lfo_phis[(val - 1) / 13107];
		saved_phase = save->phases[(val - 1) / 13107];
		saved_phase += (1.0 / 13107.0) * ((val - 1) % 13107);
	}
	double phase = start_phase + saved_phase;
	double lfo_phi = start_lfo_phi + saved_lfo_phi;

	int64_t start_time   = save->current_us;
	int64_t sustain_time = start_time + save->tick_length * ticks;
	int64_t attack_time  = start_time + 1000000LL * save->settings[SETTING_ATTACK_TIME] / 256;
	int64_t decay_time   = attack_time + 1000000LL * save->settings[SETTING_DECAY_TIME] / 256;
	int64_t release_time = sustain_time + 1000000LL * save->settings[SETTING_RELEASE_TIME] / 256;

	int64_t start_sample   =   start_time * state->sample_rate / 1000000;
	int64_t sustain_sample = sustain_time * state->sample_rate / 1000000;
	int64_t attack_sample  =  attack_time * state->sample_rate / 1000000;
	int64_t decay_sample   =   decay_time * state->sample_rate / 1000000;
	int64_t release_sample = release_time * state->sample_rate / 1000000;

	float adsr_amp = 0.0f;

	vector_f *samples = curr_samples(state);
	IndexPair range = vector_f_ensure(samples, (IndexPair){ start_sample, release_sample });
	if (note == -128) return range;
	for (int64_t i = range.begin; i < range.end; i++)
	{
		float a;
		if (i >= sustain_sample)
			a = adsr_amp - adsr_amp * (i - sustain_sample) / (release_sample - sustain_sample);
		else if (i >= decay_sample)
			a = adsr_amp = (1.0f / 65536.0f) * save->settings[SETTING_SUSTAIN_AMP];
		else if (i >= attack_sample)
		{
			float a2 = (1.0f / 65536.0f) * save->settings[SETTING_SUSTAIN_AMP];
			float a1 = (1.0f / 65536.0f) * save->settings[SETTING_ATTACK_AMP];
			a = adsr_amp = a1 + (a2 - a1) * (i - attack_sample) / (decay_sample - attack_sample);
		}
		else
		{
			float amp = (1.0f / 65536.0f) * save->settings[SETTING_ATTACK_AMP];
			a = adsr_amp = amp * (i - start_sample) / (attack_sample - start_sample);
		}
		double t = (double)(i - start_sample) / state->sample_rate;
		double p = phase_integral(t, pitch_slide, lfo_amp, lfo_omega, lfo_phi);
		vector_at(samples, i) += a * instr_amp * osc(state, my_fma(base_freq, p, phase));
	}
	double release_t = 1.0e-6 * (release_time - start_time);
	double sustain_t = 1.0e-6 * (sustain_time - start_time);
	double attack_t  = 1.0e-6 * (attack_time  - start_time);
	double decay_t   = 1.0e-6 * (decay_time   - start_time);
	if (attack_t > sustain_t) attack_t = sustain_t;
	if (decay_t  > sustain_t) decay_t  = sustain_t;
	double attack_p  = phase_integral(attack_t,  pitch_slide, lfo_amp, lfo_omega, lfo_phi);
	double decay_p   = phase_integral(decay_t,   pitch_slide, lfo_amp, lfo_omega, lfo_phi);
	double sustain_p = phase_integral(sustain_t, pitch_slide, lfo_amp, lfo_omega, lfo_phi);
	double release_p = phase_integral(release_t, pitch_slide, lfo_amp, lfo_omega, lfo_phi);
	save->phases[0] = saved_phase;
	save->phases[1] = my_fma(base_freq, attack_p,  saved_phase);
	save->phases[2] = my_fma(base_freq, decay_p,   saved_phase);
	save->phases[3] = my_fma(base_freq, sustain_p, saved_phase);
	save->phases[4] = my_fma(base_freq, release_p, saved_phase);
	save->lfo_phis[0] = saved_lfo_phi;
	save->lfo_phis[1] = my_fma(lfo_omega, attack_t,  saved_lfo_phi);
	save->lfo_phis[2] = my_fma(lfo_omega, decay_t,   saved_lfo_phi);
	save->lfo_phis[3] = my_fma(lfo_omega, sustain_t, saved_lfo_phi);
	save->lfo_phis[4] = my_fma(lfo_omega, release_t, saved_lfo_phi);
	return range;
}

static IndexPair apply_fx(State *state, uint8_t fx, uint64_t ticks)
{
	Save *save = curr_save(state);
	int64_t start_time = save->current_us;
	int64_t end_time   = start_time + save->tick_length * ticks;
	int64_t start_sample = start_time * state->sample_rate / 1000000;
	int64_t end_sample   =   end_time * state->sample_rate / 1000000;
	vector_f *samples = curr_samples(state);
	switch (fx)
	{
	case FX_LINEAR_FADE:
	{
		float amp = (1.0f / 4096.0f) * save->fxsettings[FXSETTING_LINEAR_FADE_START_AMP];
		float amp2 = (1.0f / 4096.0f) * save->fxsettings[FXSETTING_LINEAR_FADE_END_AMP];
		IndexPair range = vector_f_ensure(samples, (IndexPair){ start_sample, end_sample });
		float inc = (amp2 - amp) / (end_sample - start_sample);
		amp += (amp2 - amp) * (range.begin - start_sample) / (end_sample - start_sample);
		for (int64_t i = range.begin; i < range.end; i++)
		{
			vector_at(samples, i) *= amp;
			amp += inc;
		}
		return range;
	}
	case FX_EXP_FADE:
	{
		float amp = (1.0f / 4096.0f) * save->fxsettings[FXSETTING_EXP_FADE_START_AMP];
		float amp2 = (1.0f / 4096.0f) * save->fxsettings[FXSETTING_EXP_FADE_END_AMP];
		IndexPair range = vector_f_ensure(samples, (IndexPair){ start_sample, end_sample });
		float mul = powf(amp2 / amp, 1.0f / (end_sample - start_sample));
		amp *= powf(amp2 / amp, (range.begin - start_sample) / (end_sample - start_sample));
		for (int64_t i = range.begin; i < range.end; i++)
		{
			vector_at(samples, i) *= amp;
			amp *= mul;
		}
		return range;
	}
	case FX_AMP:
	{
		float amp = (1.0f / 4096.0f) * save->fxsettings[FXSETTING_AMP_AMP];
		IndexPair range = vector_f_ensure(samples, (IndexPair){ start_sample, end_sample });
		for (int64_t i = range.begin; i < range.end; i++)
			vector_at(samples, i) *= amp;
		return range;
	}
	case FX_CLIP:
	{
		float amp = (1.0f / 4096.0f) * save->fxsettings[FXSETTING_CLIP_AMP];
		IndexPair range = vector_f_ensure(samples, (IndexPair){ start_sample, end_sample });
		for (int64_t i = range.begin; i < range.end; i++)
		{
			float s = vector_at(samples, i);
			if (s > amp) s = amp;
			if (s < -amp) s = -amp;
			vector_at(samples, i) = s;
		}
		return range;
	}
	}
	return (IndexPair){ start_sample, start_sample };
}

static void mix_down(State *state, uint16_t tracks)
{
	vector_f *samples_dest = state->sample_stack + (state->track_stack_ptr - tracks);
	for (uint16_t i = 1; i <= tracks; i++)
	{
		vector_f *samples_src = samples_dest + i;
		IndexPair range = vector_f_ensure(samples_dest, (IndexPair){ samples_src->begin, samples_src->end });
		for (int64_t j = range.begin; j < range.end; j++)
			vector_at(samples_dest, j) += vector_at(samples_src, j);
		vector_f_clear(samples_src);
	}
	vector_frame *frames_dest = state->frame_stack + (state->track_stack_ptr - tracks);
	for (uint16_t i = 1; i <= tracks; i++)
	{
		vector_frame *frames_src = frames_dest + i;
		IndexPair range = vector_frame_ensure(frames_dest, (IndexPair){ frames_src->begin, frames_src->end });
		for (int64_t j = range.begin; j < range.end; j++)
		{
			Frame *frame_dest = &vector_at(frames_dest, j);
			Frame *frame_src = &vector_at(frames_src, j);
			uint64_t s1 = frame_dest->instr.length;
			uint64_t s2 = s1 + frame_src->instr.length;
			SimpleIndexPair range2 = simple_vector_fi_ensure(&frame_dest->instr, (SimpleIndexPair){ s1, s2 });
			for (uint64_t k = range2.begin; k < range2.end; k++)
				frame_dest->instr.data[k] = frame_src->instr.data[k - s1];
		}
		for (int64_t j = frames_src->begin; j < frames_src->end; j++)
			simple_vector_fi_destroy(&vector_at(frames_src, j).instr);
		vector_frame_clear(frames_src);
	}
	state->track_stack_ptr -= tracks;
}

static void discard(State *state, uint16_t tracks)
{
	vector_f *samples_dest = state->sample_stack + (state->track_stack_ptr - tracks);
	for (uint16_t i = 1; i <= tracks; i++)
	{
		vector_f *samples_src = samples_dest + i;
		vector_f_clear(samples_src);
	}
	vector_frame *frames_dest = state->frame_stack + (state->track_stack_ptr - tracks);
	for (uint16_t i = 1; i <= tracks; i++)
	{
		vector_frame *frames_src = frames_dest + i;
		for (int64_t j = frames_src->begin; j < frames_src->end; j++)
			simple_vector_fi_destroy(&vector_at(frames_src, j).instr);
		vector_frame_clear(frames_src);
	}
	state->track_stack_ptr -= tracks;
}

static void duplicate_push(State *state, uint64_t ticks)
{
	Save *save = curr_save(state);
	int64_t start_time = save->current_us;
	int64_t end_time   = start_time + save->tick_length * ticks;
	int64_t start_sample = start_time * state->sample_rate / 1000000;
	int64_t end_sample   =   end_time * state->sample_rate / 1000000;
	vector_f *src  = state->sample_stack + state->track_stack_ptr;
	vector_f *dest = state->sample_stack + state->track_stack_ptr + 1;
	IndexPair range_dest = vector_f_ensure(dest, (IndexPair){ start_sample, end_sample });
	IndexPair range_src  = vector_f_ensure(src, range_dest);
	for (int64_t i = range_src.begin; i < range_src.end; i++)
		vector_at(dest, i) = vector_at(src, i);
	state->track_stack_ptr++;
}

static IndexPair mark_frames(State *state, IndexPair range, uint32_t key, RGB color, int8_t note)
{
	if (range.begin >= range.end) return (IndexPair){ 0, 0 };
	vector_frame *frames = curr_frames(state);
	int64_t frame_begin = range.begin * state->fps / state->sample_rate;
	int64_t frame_end = (range.end * state->fps + state->sample_rate - 1) / state->sample_rate;
	IndexPair f_range = vector_frame_ensure(frames, (IndexPair){ frame_begin, frame_end });
	for (int64_t i = f_range.begin; i < f_range.end; i++)
	{
		uint64_t fi_begin = vector_at(frames, i).instr.length;
		uint64_t fi_end = fi_begin + state->call_stack_ptr + 1;
		SimpleIndexPair fi_range = simple_vector_fi_ensure(&vector_at(frames, i).instr, (SimpleIndexPair){ fi_begin, fi_end });
		for (uint64_t j = fi_range.begin; j < fi_range.end; j++)
		{
			uint64_t stk = j - fi_begin;
			uint64_t ip = stk == state->call_stack_ptr ? key : state->call_stack[stk] - 1;
			FrameInstr *fi = vector_at(frames, i).instr.data + j;
			fi->ip = ip;
			fi->col = color;
			fi->note = stk == 0 ? note : -128;
		}
	}
	return f_range;
}

static uint64_t prog_label_search(State *state, const uint8_t *prog, uint64_t prog_length, uint32_t label)
{
	uint64_t *node = map_u32_u64_get(&state->label_cache, label);
	if (node == NULL) return ~(uint64_t)0;
	if (*node + 1 != 0) return *node;
	while (state->label_saved < prog_length)
	{
		const uint8_t *instr = prog + (state->label_saved++) * 4;
		if (get_u8(instr, 0) != 0x10) continue;
		uint32_t prog_label = get_u24(instr, 1);
		uint64_t *nnode = map_u32_u64_get(&state->label_cache, prog_label);
		if (nnode == NULL) return ~(uint64_t)0;
		*nnode = state->label_saved;
		if (prog_label == label) return state->label_saved;
	}
	return ~(uint64_t)0;
}

static enum StepError state_step(State *state, const uint8_t *prog, uint64_t prog_length)
{
	if (state->ip >= prog_length) return STEP_EOF;
	const uint8_t *instr = prog + state->ip * 4;
	static const RGB COL_NOTE = { .r=0.75, .g=0.25, .b=0.25 };
	static const RGB COL_FX   = { .r=0.25, .g=0.75, .b=0.25 };
	Save *save = curr_save(state);
	switch (instr[0])
	{
	case 0x00:
		save->tick_length = get_u24(instr, 1);
		state->ip++;
		return STEP_SUCCESS;
	case 0x01:
	{
		int8_t note = get_s8(instr, 1);
		uint16_t ticks = get_u16(instr, 2);
		mark_frames(state, render_note(state, note, ticks), state->ip, COL_NOTE, note);
		save->current_us += (int64_t)save->tick_length * ticks;
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x02:
	{
		int8_t note = get_s8(instr, 1);
		uint16_t ticks = get_u16(instr, 2);
		mark_frames(state, render_note(state, note, ticks), state->ip, COL_NOTE, note);
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x03:
	{
		uint8_t setting = get_u8(instr, 1);
		if (setting < SETTING_COUNT)
			save->settings[setting] = get_u16(instr, 2);
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x04:
	{
		save->current_us += (int64_t)get_s24(instr, 1) * save->tick_length;
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x05:
	{
		uint8_t fxsetting = get_u8(instr, 1);
		if (fxsetting < FXSETTING_COUNT)
			save->fxsettings[fxsetting] = get_u16(instr, 2);
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x06:
	{
		uint8_t fx = get_u8(instr, 1);
		uint16_t ticks = get_u16(instr, 2);
		mark_frames(state, apply_fx(state, fx, ticks), state->ip, COL_FX, -128);
		save->current_us += (int64_t)save->tick_length * ticks;
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x07:
	{
		int8_t fx = get_s8(instr, 1);
		uint16_t ticks = get_u16(instr, 2);
		mark_frames(state, apply_fx(state, fx, ticks), state->ip, COL_FX, -128);
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x08:
	{
		uint8_t op = get_u8(instr, 1);
		uint8_t reg = op & 0x3F;
		op >>= 6;
		int16_t val = get_s16(instr, 2);
		switch (op)
		{
		case 0: save->r[reg]  = val; break;
		case 1: save->r[reg] += val; break;
		case 2: save->r[reg] -= val; break;
		case 3: save->r[reg] *= val; break;
		}
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x09:
	{
		uint8_t cond = get_u8(instr, 1);
		uint8_t reg = cond & 0x3F;
		cond >>= 6;
		int16_t jmp = get_s16(instr, 2);
		uint8_t dojmp = 0;
		switch (cond)
		{
		case 0: dojmp = save->r[reg] == 0; break;
		case 1: dojmp = save->r[reg] != 0; break;
		case 2: dojmp = save->r[reg]  > 0; break;
		case 3: dojmp = save->r[reg]  < 0; break;
		}
		if (dojmp)
			state->ip += jmp;
		else state->ip++;
		return STEP_SUCCESS;
	}
	case 0x10:
		state->ip++;
		return STEP_SUCCESS;
	case 0x11:
	{
		uint32_t packed = get_u24(instr, 1);
		uint8_t reg_note = packed & 0x3F;
		uint8_t reg_ticks = (packed >> 6) & 0x3F;
		uint16_t mult = packed >> 12;

		int8_t note = save->r[reg_note] & 0xFF;
		int64_t ticks = (int64_t)save->r[reg_ticks] * mult;
		if (ticks >= 0) mark_frames(state, render_note(state, note, ticks), state->ip, COL_NOTE, note);
		save->current_us += save->tick_length * ticks;
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x12:
	{
		uint32_t packed = get_u24(instr, 1);
		uint8_t reg_note = packed & 0x3F;
		uint8_t reg_ticks = (packed >> 6) & 0x3F;
		uint16_t mult = packed >> 12;

		int8_t note = save->r[reg_note] & 0xFF;
		int64_t ticks = (int64_t)save->r[reg_ticks] * mult;
		if (ticks >= 0) mark_frames(state, render_note(state, note, ticks), state->ip, COL_NOTE, note);
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x13:
	{
		uint8_t setting = get_u8(instr, 1);
		if (setting >= SETTING_COUNT)
		{
			state->ip++;
			return STEP_SUCCESS;
		}
		uint16_t packed = get_u16(instr, 2);
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
		int64_t val;
		if (sat && div == 0)
		{
			if (save->r[reg] < 0) val = map_a;
			else if (save->r[reg] > 0) val = map_b;
			else val = 0;
		}
		else if (!sat && sgn && div == 0)
		{
			if (save->r[reg] < 0) val = -32768;
			else if (save->r[reg] > 0) val = 32767;
			else val = 0;
		}
		else if (!sat && !sgn && div == 0)
		{
			if (save->r[reg] <= 0) val = 0;
			else val = 65535;
		}
		else if (sgn) val = ((int64_t)save->r[reg] * (1 << (sh_lut[sh] - 1))) / div;
		else val = ((int64_t)save->r[reg] * (1 << sh_lut[sh])) / div;
		if ( sat && val <  map_a) val =  map_a;
		if ( sat && val >  map_b) val =  map_b;
		if ( sgn && val < -32768) val = -32768;
		if ( sgn && val >  32767) val =  32767;
		if (!sgn && val <      0) val =      0;
		if (!sgn && val >  65535) val =  65535;
		save->settings[setting] = val;
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x14:
	{
		uint32_t packed = get_u24(instr, 1);
		uint8_t reg = packed & 0x3F;
		int64_t mult = packed >> 6;
		if (mult >= 131072) mult -= 262144;
		save->current_us += mult * save->r[reg] * save->tick_length;
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x15:
	{
		uint8_t fxsetting = get_u8(instr, 1);
		if (fxsetting >= FXSETTING_COUNT)
		{
			state->ip++;
			return STEP_SUCCESS;
		}
		uint16_t packed = get_u16(instr, 2);
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
		int64_t val;
		if (sat && div == 0)
		{
			if (save->r[reg] < 0) val = map_a;
			else if (save->r[reg] > 0) val = map_b;
			else val = 0;
		}
		else if (!sat && sgn && div == 0)
		{
			if (save->r[reg] < 0) val = -32768;
			else if (save->r[reg] > 0) val = 32767;
			else val = 0;
		}
		else if (!sat && !sgn && div == 0)
		{
			if (save->r[reg] <= 0) val = 0;
			else val = 65535;
		}
		else if (sgn) val = ((int64_t)save->r[reg] * (1 << (sh_lut[sh] - 1))) / div;
		else val = ((int64_t)save->r[reg] * (1 << sh_lut[sh])) / div;
		if ( sat && val <  map_a) val =  map_a;
		if ( sat && val >  map_b) val =  map_b;
		if ( sgn && val < -32768) val = -32768;
		if ( sgn && val >  32767) val =  32767;
		if (!sgn && val <      0) val =      0;
		if (!sgn && val >  65535) val =  65535;
		save->fxsettings[fxsetting] = val;
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x16:
	{
		uint8_t fx = get_u8(instr, 1);
		uint16_t packed = get_u16(instr, 2);
		uint8_t reg_ticks = packed & 0x3F;
		uint16_t mult = packed >> 6;
		int64_t ticks = (int64_t)save->r[reg_ticks] * mult;
		if (ticks >= 0) mark_frames(state, apply_fx(state, fx, ticks), state->ip, COL_FX, -128);
		save->current_us += (int64_t)save->tick_length * ticks;
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x17:
	{
		uint8_t fx = get_u8(instr, 1);
		uint16_t packed = get_u16(instr, 2);
		uint8_t reg_ticks = packed & 0x3F;
		uint16_t mult = packed >> 6;
		int64_t ticks = (int64_t)save->r[reg_ticks] * mult;
		if (ticks >= 0) mark_frames(state, apply_fx(state, fx, ticks), state->ip, COL_FX, -128);
		state->ip++;
		return STEP_SUCCESS;
	}
	case 0x1E:
	{
		uint32_t label = get_u24(instr, 1);
		uint64_t jmp = prog_label_search(state, prog, prog_length, label);
		if (jmp + 1 == 0) return STEP_EOF;
		state->ip = jmp;
		return STEP_SUCCESS;
	}
	case 0x1F:
	{
		if (state->call_stack_ptr >= CALL_STACK_MAX) return STEP_EOF;
		uint32_t label = get_u24(instr, 1);
		uint64_t jmp = prog_label_search(state, prog, prog_length, label);
		if (jmp + 1 == 0) return STEP_EOF;
		state->call_stack[state->call_stack_ptr++] = state->ip + 1;
		state->ip = jmp;
		return STEP_SUCCESS;
	}
	case 0x20:
	{
		uint8_t op = get_u8(instr, 1);
		uint16_t val = get_u16(instr, 2);
		switch (op)
		{
		case 0:
			if (val == 0)
			{
				if (state->track_stack_ptr + 1 >= TRACK_STACK_MAX) return STEP_EOF;
				state->track_stack_ptr++;
				state->ip++;
				return STEP_SUCCESS;
			}
			int is_discard;
			if ((is_discard = val >= 0x8000)) val = -val;
			if (val > state->track_stack_ptr) return STEP_EOF;
			if (is_discard) discard(state, val);
			else mix_down(state, val);
			state->ip++;
			return STEP_SUCCESS;
		case 1:
			if (val == 0)
			{
				if (state->save_stack_ptr + 1 >= SAVE_STACK_MAX) return STEP_EOF;
				state->save_stack[state->save_stack_ptr + 1] = state->save_stack[state->save_stack_ptr];
				state->save_stack_ptr++;
				state->ip++;
				return STEP_SUCCESS;
			}
			if (state->save_stack_ptr == 0) return STEP_EOF;
			state->save_stack_ptr--;
			state->ip++;
			return STEP_SUCCESS;
		case 2:
			if (state->track_stack_ptr + 1 >= TRACK_STACK_MAX) return STEP_EOF;
			duplicate_push(state, val);
			state->ip++;
			return STEP_SUCCESS;
		case 3:
		{
			if (val > state->track_stack_ptr) return STEP_EOF;
			vector_f *curr = state->sample_stack + state->track_stack_ptr;
			vector_f_swap(curr, curr - val);
			state->ip++;
			return STEP_SUCCESS;
		}
		default:
			state->ip++;
			return STEP_SUCCESS;
		}
	}
	case 0xFF:
	{
		if (state->call_stack_ptr == 0) return STEP_EXIT;
		state->ip = state->call_stack[--state->call_stack_ptr];
		return STEP_SUCCESS;
	}
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
		type2 tmp2 = val; \
		memcpy(&tmp, &tmp2, sizeof(type)); \
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
	const vector_f *samples = curr_samples_c(state);
	int64_t i = 0;
	for (; i < samples->begin; i++)
	{
		write_pun(file, uint32_t, float, 0);
		status->samples_saved++;
	}
	for (; i < samples->end; i++)
	{
		write_pun(file, uint32_t, float, vector_at(samples, i));
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
	RGB instr_bright[];
} RenderFrameContext;

static void rfc_get_frequency(RenderFrameContext *rfc)
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
	(void)state;
	(void)img;
	RenderFrameContext *rfc = malloc(sizeof(RenderFrameContext) + sizeof(RGB) * input->length);
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
		rfc->instr_bright[i] = (RGB){{ 0, 0, 0 }};
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
	const vector_frame *frames = curr_frames_c(state);
	const vector_f *samples = curr_samples_c(state);
	simple_vector_fi dummy = simple_vector_fi_create(0);
	simple_vector_fi *executing_instrs = t >= frames->begin ? &vector_at(frames, t).instr : &dummy;

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
	uint64_t MIN_FREQ_BIN = ceilf((float)RFC_FREQ_BINS * MIN_FREQ / input->opt.sample_rate);
	uint64_t MAX_FREQ_BIN = floorf((float)RFC_FREQ_BINS * MAX_FREQ / input->opt.sample_rate);
	if (MAX_FREQ_BIN >= RFC_FREQ_BINS) MAX_FREQ_BIN = RFC_FREQ_BINS - 1;
	int64_t samples_end = (int64_t)(t + 1) * input->opt.sample_rate / input->opt.fps;
	int64_t samples_begin = samples_end - RFC_FREQ_BINS;
	int64_t endpoint_2 = samples_end;
	if (endpoint_2 > samples->end) endpoint_2 = samples->end;
	int64_t endpoint_1 = endpoint_2;
	if (endpoint_1 > samples->begin) endpoint_1 = samples->begin;
	int64_t idx = samples_begin;
	for (; idx < endpoint_1; idx++)
		rfc->freq_real[idx - samples_begin] = 0.0f;
	for (; idx < endpoint_2; idx++)
		rfc->freq_real[idx - samples_begin] = vector_at(samples, idx);
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
		if (rx2 > RFC_FREQ_BINS) rx2 = RFC_FREQ_BINS;
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
	if (endpoint_2 > samples->end) endpoint_2 = samples->end;
	endpoint_1 = endpoint_2;
	if (endpoint_1 > samples->begin) endpoint_1 = samples->begin;
	idx = samples_begin;
	for (; idx < endpoint_1; idx++)
		rfc->freq_real[idx - samples_begin] = 0.0f;
	for (; idx < endpoint_2; idx++)
		rfc->freq_real[idx - samples_begin] = vector_at(samples, idx);
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
		for (int j = 0; j < 3; j++)
		{
			rfc->instr_bright[i].c[j] -= 12.0f / input->opt.fps;
			if (rfc->instr_bright[i].c[j] < 0.0f) rfc->instr_bright[i].c[j] = 0.0f;
		}
	}
	for (uint64_t i = 0; i < executing_instrs->length; i++)
		for (int j = 0; j < 3; j++)
			rfc->instr_bright[executing_instrs->data[i].ip].c[j] = log2(pow(2.0f, rfc->instr_bright[executing_instrs->data[i].ip].c[j]) + executing_instrs->data[i].col.c[j]);
	float rect_dilate = fminf(W, H) * (1.0f / 80.0f);
	for (uint32_t i = 0; i < input->length; i++)
	{
		int cond = 0;
		for (int j = 0; j < 3; j++)
			cond = cond || rfc->instr_bright[i].c[j] > 0.125f / input->opt.fps;
		if (cond)
		{
			float fx1 = (float)split_x1;
			float fx2 = (float)split_x2;
			int x1 = fmaxf(0, floorf(fx1 - rect_dilate));
			int x2 = fminf(W, ceilf(fx2 + rect_dilate));
			float fy1 = (float)(split_y3 - split_y2) *    i  / input->length + split_y2;
			float fy2 = (float)(split_y3 - split_y2) * (i+1) / input->length + split_y2;
			int y1 = fmaxf(0, floorf(fy1 - rect_dilate));
			int y2 = fminf(H, ceilf(fy2 + rect_dilate));
			float colr = 255.0f * rfc->instr_bright[i].c[0];
			float colg = 255.0f * rfc->instr_bright[i].c[1];
			float colb = 255.0f * rfc->instr_bright[i].c[2];
			for (int y = y1; y < y2; y++)
				for (int x = x1; x < x2; x++)
				{
					float xc = fmaxf(fx1, fminf(fx2, x));
					float yc = fmaxf(fy1, fminf(fy2, y));
					float d = hypotf(xc - x, yc - y);
					float l = fmaxf(0.0f, 1.0f - d / rect_dilate);
					img[(y*W+x)*3] = fminf(255, colr * l + img[(y*W+x)*3]);
					img[(y*W+x)*3+1] = fminf(255, colg * l + img[(y*W+x)*3+1]);
					img[(y*W+x)*3+2] = fminf(255, colb * l + img[(y*W+x)*3+2]);
				}
		}
	}
	// highlight (piano)
	const int8_t lowest_C = -9 - 12*4;
	const float highlight_radius = (float)(split_x3 - split_x2) / (octaves*10);
	for (uint64_t i = 0; i < executing_instrs->length; i++)
	{
		int8_t note = executing_instrs->data[i].note;
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
	simple_vector_fi_destroy(&dummy);
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
	size_t ppm_header_size = sprintf(ppm_header, "P6\n%" PRIu16 " %" PRIu16 "\n255\n", input->opt.width, input->opt.height);
	const vector_frame *frames = curr_frames_c(state);
	for (int64_t i = 0; i < frames->end; i++)
	{
		render_frame(rfc, state, input, img, i);
		sprintf(filename, "%s/frame%" PRId64 ".ppm", input->opt.frames_dir, i);
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
	const vector_f *samples = curr_samples_c(state);
	const vector_frame *frames = curr_frames_c(state);
	int64_t audio_delay = samples->begin;
	if (audio_delay < 0) audio_delay = 0;
	int64_t audio_length = samples->end - audio_delay;
	if (audio_length < 0) audio_length = 0;
	if (!input->opt.generate_video)
	{
		int pipefd[2];
		if (pipe(pipefd)) return RENDER_STATUS_ERR_AUDIO;
		char s[MAX_ARGS_LENGTH*2 + 256];
		sprintf(s, "ffmpeg -y -loglevel quiet -f f32%s -ar %d -ac 1 -i pipe:%d -filter:a \"adelay=delays=%" PRId64 "S:all=1\" %s %s",
			little_endian() ? "le" : "be",
			input->opt.sample_rate,
			pipefd[0],
			audio_delay,
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
			sizeof(float) * audio_length,
			8192,
			audio_length > 0 ? &vector_at(samples, audio_delay) : NULL,
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
	char s[MAX_ARGS_LENGTH*4 + 300];
	int s_len = sprintf(s, "ffmpeg -y -loglevel quiet -f f32%s -ar %" PRIu32 " -ac 1 -i pipe:%d -f rawvideo -pixel_format rgb24 -video_size %" PRIu16 "x%" PRIu16 " -framerate %" PRIu16 " -i pipe:%d -filter_complex \"[0:a]adelay=delays=%" PRId64 "S:all=1,asplit=%s\" -map \"[av]\" -map 1:v %s %s",
		little_endian() ? "le" : "be",
		input->opt.sample_rate,
		pipe_a[0],
		input->opt.width,
		input->opt.height,
		input->opt.fps,
		pipe_v[0],
		audio_delay,
		input->opt.generate_audio ? "2[av][aa]" : "1[av]",
		input->opt.ffmpeg_args_video,
		input->opt.video_filename);
	if (input->opt.generate_audio)
		sprintf(s + s_len, " -map \"[aa]\" %s %s", input->opt.ffmpeg_args_audio, input->opt.audio_filename);
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
		sizeof(float) * audio_length,
		8192,
		audio_length > 0 ? &vector_at(samples, audio_delay) : NULL,
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
	for (int64_t i = 0; i < frames->end; i++)
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
	state_init(&state, input->opt.sample_rate, input->opt.fps, input->opt.preroll_max, input->opt.time_max);
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
		const vector_f *samples = curr_samples_c(&state);
		const vector_frame *frames = curr_frames_c(&state);
		if (samples->end >= 0 && input->ctx->status.samples_estimate <= (uint64_t)samples->end)
			input->ctx->status.samples_estimate = (uint64_t)samples->end;
		if (frames->end >= 0 && input->ctx->status.frames_estimate <= (uint64_t)frames->end)
			input->ctx->status.frames_estimate = (uint64_t)frames->end;
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
