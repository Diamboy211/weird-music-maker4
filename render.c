#include <stdlib.h>
#include <inttypes.h>
#include <ncurses.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "render.h"
#include "render_internal.h"

void render_options_default(RenderOptions *opt)
{
	strncpy(opt->ffmpeg_bin, "/usr/bin/ffmpeg", MAX_ARGS_LENGTH + 1);
	memset(opt->ffmpeg_args_audio, 0, MAX_ARGS_LENGTH + 1);
	strncpy(opt->ffmpeg_args_video, "-c:v libx264 -c:a aac -b:a 192k -pix_fmt yuv420p -movflags +faststart", MAX_ARGS_LENGTH + 1);
	strncpy(opt->video_filename, "video.mp4", MAX_ARGS_LENGTH + 1);
	strncpy(opt->audio_filename, "audio.wav", MAX_ARGS_LENGTH + 1);
	strncpy(opt->frames_dir, "frames/", MAX_ARGS_LENGTH + 1);

	opt->sample_rate      = 44100;
	opt->instructions_max = 10000;
	opt->fps              = 24;
	opt->time_max         = 600;
	opt->width            = 640;
	opt->height           = 360;
	opt->use_ffmpeg       = 1;
	opt->generate_video   = 0;
	opt->generate_audio   = 1;
}

static void write_option_bool(const char *prefix, const void *value, const char *suffix, uint8_t highlight)
{
	int flag_s = A_DIM;
	int flag_v = A_DIM;
	if (highlight)
	{
		flag_s = A_NORMAL;
		flag_v = A_NORMAL | A_REVERSE;
	}
	attrset(flag_s);
	addstr(prefix);
	attrset(flag_v);
	addstr(*(const uint8_t *)value ? "yes" : "no");
	attrset(flag_s);
	addstr(suffix);
}

#define def_wo(fmt, type, suf) static void write_option_##suf(const char *prefix, const void *value, const char *suffix, uint8_t highlight) \
{ \
	int flag_s = A_DIM; \
	int flag_v = A_DIM; \
	if (highlight) \
	{ \
		flag_s = A_NORMAL; \
		flag_v = A_NORMAL | A_REVERSE; \
	} \
	attrset(flag_s); \
	addstr(prefix); \
	attrset(flag_v); \
	printw("%" fmt, *(const type *)value); \
	attrset(flag_s); \
	addstr(suffix); \
}

def_wo(PRIu16, uint16_t, u16)
def_wo(PRIu32, uint32_t, u32)

static void write_option_string(const char *prefix, const void *value, const char *suffix, uint8_t highlight)
{
	int flag_s = A_DIM;
	int flag_v = A_DIM;
	if (highlight)
	{
		flag_s = A_NORMAL;
		flag_v = A_NORMAL | A_REVERSE;
	}
	attrset(flag_s);
	addstr(prefix);
	attrset(flag_v);
	addstr(value);
	attrset(flag_s);
	addstr(suffix);
}

static void write_option_est(const char *prefix, const void *value, const char *suffix, uint8_t highlight)
{
	const RenderOptions *opt = value;
	uint64_t est = 0;
	if (opt->generate_video)
		est += 3ULL * opt->width * opt->height * opt->fps;
	if (opt->generate_audio)
		est += sizeof(float) * opt->sample_rate;
	est *= opt->time_max;
	char size_suf = 'B';
	uint64_t size_div = 1;
	if (est > (1ULL << 10)) size_suf = 'K', size_div <<= 10;
	if (est > (1ULL << 20)) size_suf = 'M', size_div <<= 10;
	if (est > (1ULL << 30)) size_suf = 'G', size_div <<= 10;
	if (est > (1ULL << 40)) size_suf = 'T', size_div <<= 10;
	int flag = A_DIM;
	if (highlight) flag = A_NORMAL | A_REVERSE;
	attrset(flag);
	addstr(prefix);
	if (opt->use_ffmpeg) addstr("???");
	else printw("%.3f%c", (float)est / size_div, size_suf);
	addstr(suffix);
}

static int change_option_reset(RenderOptions *opt, void *data)
{
	(void)data;
	render_options_default(opt);
	return 0;
}

static int change_option_bool(RenderOptions *opt, void *data)
{
	(void)opt;
	uint8_t *b = data;
	*b = !*b;
	return 0;
}

#define def_co(pfmt, sfmt, type, suf) static int change_option_##suf(RenderOptions *opt, void *data) \
{ \
	(void)opt; \
	int w, h; \
	getmaxyx(stdscr, h, w); \
	attrset(A_REVERSE); \
	move(h-1, 0); \
	for (int i = 0; i < w; i++) addch(' '); \
	mvaddstr(h-1, 0, "enter number: "); \
	char s[24]; \
	snprintf(s, 23, "%" pfmt, *(type *)data); \
	curs_set(2); \
	nocbreak(); \
	echo(); \
	char *it = s; \
	while (*it) it++; \
	while (it != s) ungetch(*--it); \
	scanw("%" sfmt, (type *)data); \
	noecho(); \
	cbreak(); \
	curs_set(0); \
	attrset(A_NORMAL); \
	return 0; \
}

def_co(PRIu16, SCNu16, uint16_t, u16)
def_co(PRIu32, SCNu32, uint32_t, u32)

static int change_option_string(RenderOptions *opt, void *data)
{
	(void)opt;
	int w, h;
	getmaxyx(stdscr, h, w);
	attrset(A_REVERSE);
	move(h-1, 0);
	for (int i = 0; i < w; i++) addch(' ');
	mvaddstr(h-1, 0, "enter string: ");
	char *s = data;
	curs_set(2);
	nocbreak();
	echo();
	char *it = s;
	while (*it) it++;
	while (it != s) ungetch(*--it);
	getnstr(s, MAX_ARGS_LENGTH);
	noecho();
	cbreak();
	curs_set(0);
	attrset(A_NORMAL);
	return 0;
}

static int change_r0(RenderOptions *opt, void *data)
{
	(void)opt;
	(void)data;
	return 0;
}

static int change_r1(RenderOptions *opt, void *data)
{
	(void)opt;
	(void)data;
	return 1;
}

static int change_r2(RenderOptions *opt, void *data)
{
	(void)opt;
	(void)data;
	return 2;
}

typedef struct
{
	void (*print)(const char *, const void *, const char *, uint8_t);
	int (*press)(RenderOptions *, void *);
	char *prefix;
	void *data;
	char *suffix;
} Option;

int render_options_set(RenderOptions *opt)
{
	RenderOptions new_opt;
	memcpy(&new_opt, opt, sizeof(RenderOptions));
	Option options_table[] = {
		{ write_option_string, change_option_reset,  "", "reset to defaults", "\n" },
		{ write_option_bool,   change_option_bool,   "use ffmpeg: ", &new_opt.use_ffmpeg, "\n" },
		{ write_option_bool,   change_option_bool,   "generate audio file: ", &new_opt.generate_audio, "\n" },
		{ write_option_bool,   change_option_bool,   "generate video: ", &new_opt.generate_video, "\n" },
		{ write_option_string, change_option_string, "ffmpeg binary: ", new_opt.ffmpeg_bin, "\n" },
		{ write_option_string, change_option_string, "ffmpeg audio arguments: ", new_opt.ffmpeg_args_audio, "\n" },
		{ write_option_string, change_option_string, "ffmpeg video arguments: ", new_opt.ffmpeg_args_video, "\n" },
		{ write_option_string, change_option_string, "(ffmpeg) video file: ", new_opt.video_filename, "\n" },
		{ write_option_string, change_option_string, "(no ffmpeg) frames directory: ", new_opt.frames_dir, "\n" },
		{ write_option_string, change_option_string, "audio file: ", new_opt.audio_filename, "\n" },
		{ write_option_u32,    change_option_u32,    "audio sample rate: ", &new_opt.sample_rate, "\n" },
		{ write_option_u16,    change_option_u16,    "video fps: ", &new_opt.fps, "\n" },
		{ write_option_u16,    change_option_u16,    "video width: ", &new_opt.width, "\n" },
		{ write_option_u16,    change_option_u16,    "video height: ", &new_opt.height, "\n" },
		{ write_option_u32,    change_option_u32,    "maximum duration: ", &new_opt.time_max, "s\n" },
		{ write_option_u32,    change_option_u32,    "instruction execution limit: ", &new_opt.instructions_max, "\n" },
		{ write_option_est,    change_r0,            "(read only) estimated disk space: ", &new_opt, "\n" },
		{ write_option_string, change_r1,            "", "confirm", "\n" },
		{ write_option_string, change_r2,            "", "cancel", "\n" },
	};
	const int options_length = sizeof(options_table) / sizeof(Option);
	def_prog_mode();
	cbreak();
	noecho();
	int cursor_y = 0;
	uint8_t cont = 1;
	int ret = 0;
	while (cont)
	{
		erase();
		move(0, 0);
		for (int i = 0; i < options_length; i++)
			options_table[i].print(
				options_table[i].prefix,
				options_table[i].data,
				options_table[i].suffix,
				i == cursor_y);
		refresh();
		switch (getch())
		{
		case 'k':
		case KEY_UP:
			cursor_y--;
			if (cursor_y < 0) cursor_y = 0;
			break;
		case 'j':
		case KEY_DOWN:
			cursor_y++;
			if (cursor_y > options_length - 1) cursor_y = options_length - 1;
			break;
		case '\n':
		case 'z':
			if ((ret = options_table[cursor_y].press(&new_opt, options_table[cursor_y].data) - 1) != -1)
				cont = 0;
			break;
		default: break;
		}
	}
	if (ret == 0) memcpy(opt, &new_opt, sizeof(RenderOptions));
	reset_prog_mode();
	return ret;
}

void *render_thread(void *data)
{
	render((RenderInput *)data);
	return NULL;
}

RenderContext *render_start(const List *data, const RenderOptions *opt)
{
	RenderInput *input = malloc(sizeof(RenderInput));
	if (input == NULL) return NULL;
	input->length = data->length;
	input->instr = malloc(data->length * 4);
	if (input->instr == NULL)
	{
		free(input);
		return NULL;
	}
	ListNode *it = data->front;
	for (uint64_t i = 0; i < (uint64_t)data->length * 4; i += 4)
	{
		memcpy(input->instr+i, it->e, 4);
		it = it->next;
	}
	memcpy(&input->opt, opt, sizeof(RenderOptions));
	input->ctx = malloc(sizeof(RenderContext));
	if (input->ctx == NULL)
	{
		free(input->instr);
		free(input);
		return NULL;
	}
	memset(input->ctx, 0, sizeof(RenderContext));
	if (pthread_create(&input->ctx->thread_id, NULL, render_thread, input))
	{
		free(input->instr);
		free(input->ctx);
		free(input);
		return NULL;
	}
	return input->ctx;
}

void render_status(RenderStatus *status, const RenderContext *opt)
{
	memcpy(status, &opt->status, sizeof(RenderStatus));
}
void render_stop(RenderContext *ctx)
{
	pthread_t thread_id = ctx->thread_id;
	ctx->terminate = 1;
	pthread_join(thread_id, NULL);
}
