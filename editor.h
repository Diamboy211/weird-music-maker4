#pragma once

#include <stdint.h>
#include <ncurses.h>
#include <time.h>
#include "list.h"
#include "render.h"

#define MAX_FILENAME_LENGTH 255

typedef struct
{
	RenderOptions render_options;
	char filename[MAX_FILENAME_LENGTH+1];
	List data, clipboard;
	struct timespec render_start;
	ListNode *cursor, *visual_anchor;
	WINDOW *hex, *info, *footer, *render;
	RenderContext *render_context;
	int window_w;
	int window_h;
	int window_y;
	int padding;
	int cursor_y;
	int cursor_x;
	int visual_y;
	uint8_t named_file;
	uint8_t drawn;
	uint8_t visual;
	uint8_t saved;
	uint8_t rendering;
	uint8_t rendering_error;
	uint8_t accidental;
	uint8_t label_mode;
} Editor;

int editor_create(Editor *editor);
int editor_load(Editor *editor, const char *filename);
int editor_init_display(Editor *editor);
int editor_tick(Editor *editor);
int editor_destroy(Editor *editor);
