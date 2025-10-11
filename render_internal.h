#pragma once

#include <stdint.h>
#include <pthread.h>
#include "render.h"

typedef struct
{
	RenderOptions opt;
	RenderContext *ctx;
	uint64_t length;
	uint8_t *instr;
} RenderInput;

struct RenderContext
{
	RenderStatus status;
	pthread_t thread_id;
	uint32_t error;
	uint8_t wait;
	uint8_t terminate;
};

void render(RenderInput *input);
