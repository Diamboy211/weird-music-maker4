#pragma once

#include <stdint.h>
#include <stdatomic.h>
#include "list.h"

#define MAX_ARGS_LENGTH 255

static const uint8_t RENDER_STATUS_ERR_AUDIO = 1;
static const uint8_t RENDER_STATUS_ERR_VIDEO = 2;
static const uint8_t RENDER_STATUS_ERR_EOF   = 4;
static const uint8_t RENDER_STATUS_ERR_UNK   = 8;
static const uint8_t RENDER_STATUS_ERR_MAX   = 16;
static const uint8_t RENDER_STATUS_ERR_EARLY = 32;

typedef struct
{
	char ffmpeg_bin[MAX_ARGS_LENGTH + 1];
	char ffmpeg_args_audio[MAX_ARGS_LENGTH + 1];
	char ffmpeg_args_video[MAX_ARGS_LENGTH + 1];
	char video_filename[MAX_ARGS_LENGTH + 1];
	char audio_filename[MAX_ARGS_LENGTH + 1];
	char frames_dir[MAX_ARGS_LENGTH + 1];

	uint32_t sample_rate;
	uint32_t instructions_max;

	uint32_t time_max;
	uint16_t fps;
	uint16_t width, height;

	uint8_t use_ffmpeg;
	uint8_t generate_audio;
	uint8_t generate_video;
} RenderOptions;

typedef struct
{
	_Atomic uint64_t frames_saved;
	_Atomic uint64_t frames_estimate;
	_Atomic uint64_t samples_saved;
	_Atomic uint64_t samples_estimate;
	_Atomic uint8_t error;
	_Atomic uint8_t done;
} RenderStatus;

typedef struct RenderContext RenderContext;

void render_options_default(RenderOptions *opt);
int render_options_set(RenderOptions *opt);
RenderContext *render_start(const List *data, const RenderOptions *opt);
void render_status(RenderStatus *status, const RenderContext *opt);
void render_stop(RenderContext *ctx);
