#pragma once

#include <stdint.h>
#include <stdatomic.h>
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
	uint8_t wait;
	volatile _Atomic uint8_t terminate;
};

void render(RenderInput *input);
