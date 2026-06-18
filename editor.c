#include <stdio.h>
#include <locale.h>
#include <string.h>
#include <inttypes.h>
#include "editor.h"
#include "instr.h"

static const uint8_t DRAWN_HEX    = 1;
static const uint8_t DRAWN_INFO   = 2;
static const uint8_t DRAWN_FOOTER = 4;
static const uint8_t DRAWN_RENDER = 8;
static const uint8_t VISUAL_ON    = 1;
static const uint8_t VISUAL_DIR   = 2;

int editor_create(Editor *editor)
{
	memset(editor, 0, sizeof(Editor));
	render_options_default(&editor->render_options);
	return 0;
}

int editor_init_display(Editor *editor)
{
	(void)editor;
	setlocale(LC_CTYPE, "");
	initscr();
	halfdelay(1);
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	refresh();
	return 0;
}

static void editor_uncurses()
{
	def_prog_mode();
	endwin();
}

static void editor_recurses_wait()
{
	fputs("press enter to continue", stderr);
	getchar();
	reset_prog_mode();
	refresh();
}

static void editor_recurses()
{
	reset_prog_mode();
	refresh();
}

int editor_load(Editor *editor, const char *filename)
{
	editor_uncurses();
	FILE *file = fopen(filename, "rb");
	List data_new;
	list_create(&data_new);
	// new file
	if (file == NULL)
	{
		fprintf(stderr, "%s: new file\n", filename);
		strncpy(editor->filename, filename, MAX_FILENAME_LENGTH);
		editor->named_file = 1;
		list_swap(&editor->data, &data_new);
		list_destroy(&data_new);
		editor_recurses();
		return 0;
	}
	uint8_t buf[4];
	if (fread(buf, 1, 4, file) < 4)
	{
		editor->named_file = 0;
		if (feof(file))
			fprintf(stderr, "%s: unexpected EOF\n", filename);
		else if (ferror(file))
			perror(filename);
		fclose(file);
		editor_recurses_wait();
		return 1;
	}
	if (memcmp(buf, "dbs4", 4) != 0)
	{
		editor->named_file = 0;
		fprintf(stderr, "%s: not a dbs4 file\n", filename);
		fclose(file);
		editor_recurses_wait();
		return 1;
	}
	if (fread(buf, 1, 4, file) < 4)
	{
		editor->named_file = 0;
		if (feof(file))
			fprintf(stderr, "%s: unexpected EOF\n", filename);
		else if (ferror(file))
			perror(filename);
		fclose(file);
		editor_recurses_wait();
		return 1;
	}
	uint32_t length = ((uint32_t)buf[3] << 24)
		| ((uint32_t)buf[2] << 16)
		| ((uint32_t)buf[1] << 8)
		| buf[0];
	uint32_t read = 0;
	for (; read < length; read++)
	{
		if (fread(buf, 1, 4, file) < 4) break;
		list_insert_after(&data_new, data_new.back, buf);
	}
	if (read == length)
	{
		editor->named_file = 1;
		editor->saved      = 1;
		strncpy(editor->filename, filename, MAX_FILENAME_LENGTH);
		list_swap(&editor->data, &data_new);
		list_destroy(&data_new);
		fclose(file);
		editor_recurses();
		return 0;
	}
	if (feof(file))
	{
		fprintf(stderr, "%s: unexpected EOF\nload anyway? (y/N): ", filename);
		if (getchar() == 'y')
		{
			editor->named_file = 1;
			strncpy(editor->filename, filename, MAX_FILENAME_LENGTH);
			list_swap(&editor->data, &data_new);
			list_destroy(&data_new);
			editor_recurses();
			return 0;
		}
		editor->named_file = 0;
		list_destroy(&data_new);
		fclose(file);
		editor_recurses();
		return 1;
	}
	perror(filename);
	fputs("load anyway? (y/N): ", stderr);
	if (getchar() == 'y')
	{
		editor->named_file = 1;
		strncpy(editor->filename, filename, MAX_FILENAME_LENGTH);
		list_swap(&editor->data, &data_new);
		list_destroy(&data_new);
		fclose(file);
		editor_recurses();
		return 0;
	}
	editor->named_file = 0;
	list_destroy(&data_new);
	fclose(file);
	editor_recurses();
	return 1;
}

static void editor_redraw_hex(Editor *editor)
{
	if (editor->drawn & DRAWN_HEX) return;
	werase(editor->hex);
	int h = getmaxy(editor->hex);
	mvwvline(editor->hex, 0, 1, ACS_VLINE, h);
	mvwvline(editor->hex, 0, 13, ACS_VLINE, h);
	if (editor->data.length == 0)
	{
		wattron(editor->hex, A_DIM);
		mvwaddstr(editor->hex, 0, 2, "press 'a'");
		mvwaddstr(editor->hex, 1, 2, "to add an");
		mvwaddstr(editor->hex, 2, 2, "instruction");
		wattroff(editor->hex, A_DIM);
	}
	int y = editor->cursor_y - editor->window_y;
	ListNode *it = editor->cursor;
	if (it)
	{
		int vy1 = (editor->visual & VISUAL_DIR) ? editor->cursor_y : editor->visual_y;
		int vy2 = (editor->visual & VISUAL_DIR) ? editor->visual_y : editor->cursor_y;
		for (; it->prev && y > 0; it = it->prev, y--);
		for (; it && y < h; it = it->next, y++)
		{
			int offset = editor->window_y + y - editor->cursor_y;
			if (offset < 0) offset = -offset;
			mvwaddch(editor->hex, y, 0, '0' + offset % 10);
			for (int i = 0; i < 8; i++)
			{
				const char *hex_table = "0123456789ABCDEF";
				int x = i * 3 / 2 + 2;
				int e = it->e[i/2];
				e >>= 4 * (1 - i%2);
				e &= 0x0F;
				int flag = A_DIM;
				if (editor->window_y + y == editor->cursor_y)
				{
					flag = A_NORMAL;
					if (editor->cursor_x == i)
						flag = A_REVERSE;
				}
				if ((editor->visual & VISUAL_ON)
					&& editor->window_y + y >= vy1
					&& editor->window_y + y <= vy2)
					flag ^= A_NORMAL | A_REVERSE;
				mvwaddch(editor->hex, y, x, hex_table[e] | flag);
			}
		}
	}
	editor->drawn |= DRAWN_HEX;
	wnoutrefresh(editor->hex);
}

static void editor_redraw_info(Editor *editor)
{
	if (editor->drawn & DRAWN_INFO) return;
	werase(editor->info);
	int w, h;
	getmaxyx(editor->info, h, w);
	wattrset(editor->info, A_NORMAL);
	mvwvline(editor->info, 0, w-1, ACS_VLINE, h);
	int y = editor->cursor_y - editor->window_y;
	char s[w+2];
	ListNode *it = editor->cursor;
	if (it)
	{
		for (; it->prev && y > 0; it = it->prev, y--);
		for (; it && y < h; it = it->next, y++)
		{
			int flag = A_DIM;
			if (editor->window_y + y == editor->cursor_y)
				flag = A_NORMAL;
			wattrset(editor->info, flag);
			int info_print = snfmtinstr(s, w+1, editor, it);
			if (info_print <= w-1)
				mvwaddstr(editor->info, y, 0, s);
			else
			{
				mvwaddnstr(editor->info, y, 0, s, w-4);
				waddstr(editor->info, "...");
			}
		}
	}
	editor->drawn |= DRAWN_INFO;
	wnoutrefresh(editor->info);
}

static void editor_redraw_footer(Editor *editor)
{
	if (editor->drawn & DRAWN_FOOTER) return;
	werase(editor->footer);
	int w = getmaxx(editor->footer);
	char progress[32];
	int progress_length = snprintf(progress, 32, "%d/%d",
			editor->cursor_y, (int)editor->data.length);
	mvwaddstr(editor->footer, 0, w - progress_length, progress);
	const char *name = editor->named_file ? editor->filename : "[new]";
	int name_max     = w - progress_length - 1;
	int name_length  = strlen(name);
	wmove(editor->footer, 0, 0);
	if (editor->saved == 0)
	{
		waddch(editor->footer, '*');
		name_max--;
	}
	if (name_length <= name_max)
		waddstr(editor->footer, name);
	else
	{
		int name_print = name_max - 3;
		if (name_print < 0) name_print = 0;
		waddnstr(editor->footer, name, name_print);
		int dot_print = name_max - name_print;
		if (dot_print < 0) dot_print = 0;
		waddnstr(editor->footer, "...", dot_print);
	}
	editor->drawn |= DRAWN_FOOTER;
	mvwchgat(editor->footer, 0, 0, -1, A_REVERSE, 0, NULL);
	wnoutrefresh(editor->footer);
}

static void editor_redraw_render(Editor *editor)
{
	if (editor->drawn & DRAWN_RENDER) return;
	werase(editor->render);
	wmove(editor->render, 0, 0);
	if (!editor->rendering)
		waddstr(editor->render, "done\n");
	else
	{
		RenderStatus status;
		render_status(&status, editor->render_context);
		wprintw(editor->render, "samples estimate: %" PRIu64 "\n", status.samples_estimate);
		wprintw(editor->render, "frames estimate: %" PRIu64 "\n", status.frames_estimate);
		wprintw(editor->render, "samples saved: %" PRIu64 "\n", status.samples_saved);
		wprintw(editor->render, "frames saved: %" PRIu64 "\n", status.frames_saved);
	}
	if (editor->rendering_error == 0)
		waddstr(editor->render, "no errors");
	else
	{
		waddstr(editor->render, "errors:");
		if (editor->rendering_error & RENDER_STATUS_ERR_AUDIO) waddstr(editor->render, "\naudio");
		if (editor->rendering_error & RENDER_STATUS_ERR_VIDEO) waddstr(editor->render, "\nvideo");
		if (editor->rendering_error & RENDER_STATUS_ERR_EOF)   waddstr(editor->render, "\nout-of-bound execution");
		if (editor->rendering_error & RENDER_STATUS_ERR_UNK)   waddstr(editor->render, "\nunk instruction");
		if (editor->rendering_error & RENDER_STATUS_ERR_MAX)   waddstr(editor->render, "\nhit max instructions");
		if (editor->rendering_error & RENDER_STATUS_ERR_EARLY) waddstr(editor->render, "\nearly termination");
	}
	editor->drawn |= DRAWN_FOOTER;
	wnoutrefresh(editor->render);
}

static int editor_validate(Editor *editor)
{
	int w, h;
	getmaxyx(stdscr, h, w);
	if (w < 20 || h < 4)
	{
		mvaddstr(0, 0, "20x4 @least");
		refresh();
		getch();
		return 0;
	}
	if (editor->window_w != w || editor->window_h != h)
	{
		if (editor->hex)    delwin(editor->hex);
		if (editor->info)   delwin(editor->info);
		if (editor->footer) delwin(editor->footer);
		if (editor->render) delwin(editor->render);
		int x1 = 14;
		int x2 = w/2 + 7;
		int xr = w - 28;
		if (x2 < xr) x2 = xr;
		editor->hex      = newwin(h-1,    x1,   0,  0);
		editor->info     = newwin(h-1, x2-x1,   0, x1);
		editor->footer   = newwin(  0,     0, h-1,  0);
		editor->render   = newwin(h-1,     0,   0, x2);
		editor->window_w = w;
		editor->window_h = h;
		editor->padding  = h / 4;
		editor->drawn    = 0;
		refresh();
	}
	if (editor->cursor == NULL && editor->data.length)
	{
		editor->cursor   = editor->data.front;
		editor->cursor_y = 1;
		editor->window_y = 1;
	}
	if (editor->cursor_x < 0) editor->cursor_x = 0;
	if (editor->cursor_x > 7) editor->cursor_x = 7;
	if (editor->data.length)
	{
		if (editor->cursor_y - editor->window_y < editor->padding)
		{
			editor->window_y = editor->cursor_y - editor->padding;
			if (editor->window_y < 1)
				editor->window_y = 1;
			editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO);
		}
		if (editor->window_y + editor->window_h - 2 - editor->cursor_y < editor->padding)
		{
			editor->window_y = editor->cursor_y - editor->window_h + editor->padding + 2;
			if (editor->window_y > editor->data.length - editor->window_h + 2)
				editor->window_y = editor->data.length - editor->window_h + 2;
			editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO);
		}
	}
	if (editor->rendering)
	{
		RenderStatus status;
		render_status(&status, editor->render_context);
		editor->rendering_error = status.error;
		if (status.done)
		{
			render_stop(editor->render_context);
			editor->render_context = NULL;
			editor->rendering = 0;
		}
		editor->drawn &= ~DRAWN_RENDER;
	}
	return 0;
}

static int editor_redraw(Editor *editor)
{
	editor_redraw_hex(editor);
	editor_redraw_info(editor);
	editor_redraw_footer(editor);
	editor_redraw_render(editor);
	doupdate();
	return 0;
}

static void editor_get_filename(Editor *editor, char *dest, const char *str)
{
	curs_set(2);
	nocbreak();
	echo();
	wmove(editor->footer, 0, 0);
	wclrtoeol(editor->footer);
	waddstr(editor->footer, str);
	wgetnstr(editor->footer, dest, MAX_FILENAME_LENGTH);
	noecho();
	halfdelay(1);
	curs_set(0);
	editor->drawn &= ~DRAWN_FOOTER;
}

static void editor_save(Editor *editor, uint8_t rename)
{
	char filename[MAX_FILENAME_LENGTH+1];
	memset(filename, 0, sizeof(filename));
	if (rename || !editor->named_file)
		editor_get_filename(editor, filename, "save as: ");
	else strcpy(filename, editor->filename);
	// if (strlen(filename) == 0)
	if (*filename == 0) return;
	editor_uncurses();
	FILE *file = fopen(filename, "wb");
	if (file == NULL)
	{
		perror(filename);
		editor_recurses_wait();
		return;
	}
	if (fwrite("dbs4", 1, 4, file) < 4)
	{
		perror(filename);
		fclose(file);
		editor_recurses_wait();
		return;
	}
	uint8_t length_bytes[4];
	length_bytes[0] = editor->data.length;
	length_bytes[1] = editor->data.length >> 8;
	length_bytes[2] = editor->data.length >> 16;
	length_bytes[3] = editor->data.length >> 24;
	if (fwrite(length_bytes, 1, 4, file) < 4)
	{
		perror(filename);
		fclose(file);
		editor_recurses_wait();
		return;
	}
	for (ListNode *it = editor->data.front; it; it = it->next)
		if (fwrite(it->e, 1, 4, file) < 4)
		{
			perror(filename);
			fclose(file);
			editor_recurses_wait();
			return;
		}
	fclose(file);
	editor_recurses();
	memcpy(editor->filename, filename, MAX_FILENAME_LENGTH+1);
	editor->saved = 1;
	editor->named_file = 1;
}

static void editor_load2(Editor *editor)
{
	char filename[MAX_FILENAME_LENGTH+1];
	memset(filename, 0, sizeof(filename));
	editor_get_filename(editor, filename, "load: ");
	if (*filename == 0) return;
	editor->window_y = 0;
	editor->cursor = NULL;
	editor->cursor_y = 0;
	editor_load(editor, filename);
	editor->drawn = 0;
}

static void editor_visual_off(Editor *editor)
{
	editor->visual_anchor = NULL;
	editor->visual        = 0;
	editor->visual_y      = 0;
}

static void editor_visual_on(Editor *editor)
{
	editor->visual_anchor = editor->cursor;
	editor->visual        = VISUAL_ON;
	editor->visual_y      = editor->cursor_y;
}

static int editor_inputs(Editor *editor)
{
	static const uint8_t default_instr[4] = {0,0,0,0};
	int ch = getch();
	switch (ch)
	{
	case 'q':
		if (editor->saved == 0) editor_save(editor, 0);
		return 1;
	case 'Q':
		return 1;
	case 'h':
	case KEY_LEFT:
		editor->cursor_x--;
		editor->drawn &= ~DRAWN_HEX;
		return 0;
	case 'l':
	case KEY_RIGHT:
		editor->cursor_x++;
		editor->drawn &= ~DRAWN_HEX;
		return 0;
	case KEY_HOME:
		editor->cursor_x = 0;
		editor->drawn &= ~DRAWN_HEX;
		return 0;
	case KEY_END:
		editor->cursor_x = 7;
		editor->drawn &= ~DRAWN_HEX;
		return 0;
	case 'k':
	case KEY_UP:
		if (editor->cursor == NULL) return 0;
		if (editor->cursor->prev == NULL) return 0;
		editor->cursor_y--;
		if ((editor->visual & VISUAL_ON) && editor->cursor == editor->visual_anchor)
			editor->visual |= VISUAL_DIR;
		editor->cursor = editor->cursor->prev;
		editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO | DRAWN_FOOTER);
		return 0;
	case 'j':
	case KEY_DOWN:
		if (editor->cursor == NULL) return 0;
		if (editor->cursor->next == NULL) return 0;
		editor->cursor_y++;
		editor->cursor = editor->cursor->next;
		if ((editor->visual & VISUAL_ON) && editor->cursor == editor->visual_anchor)
			editor->visual &= ~VISUAL_DIR;
		editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO | DRAWN_FOOTER);
		return 0;
	case KEY_PPAGE:
		if (editor->cursor == NULL) return 0;
		if (editor->cursor->prev == NULL) return 0;
		for (int i = 0; i < editor->window_h; i++)
		{
			if (!editor->cursor->prev) break;
			editor->cursor_y--;
			if ((editor->visual & VISUAL_ON) && editor->cursor == editor->visual_anchor)
				editor->visual |= VISUAL_DIR;
			editor->cursor = editor->cursor->prev;
		}
		editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO | DRAWN_FOOTER);
		return 0;
	case KEY_NPAGE:
		if (editor->cursor == NULL) return 0;
		if (editor->cursor->next == NULL) return 0;
		for (int i = 0; i < editor->window_h; i++)
		{
			if (!editor->cursor->next) break;
			editor->cursor_y++;
			if ((editor->visual & VISUAL_ON) && editor->cursor == editor->visual_anchor)
				editor->visual &= ~VISUAL_DIR;
			editor->cursor = editor->cursor->next;
		}
		editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO | DRAWN_FOOTER);
		return 0;
	case 'z':
	{
		if (editor->cursor == NULL) return 0;
		int byte   = editor->cursor_x / 2;
		int nibble = editor->cursor_x % 2;
		editor->cursor->e[byte] += nibble ? 0x01 : 0x10;
		editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO | DRAWN_FOOTER);
		editor->saved = 0;
		return 0;
	}
	case 'Z':
	{
		if (editor->cursor == NULL) return 0;
		int byte   = editor->cursor_x / 2;
		int nibble = editor->cursor_x % 2;
		editor->cursor->e[byte] += nibble ? 0x04 : 0x40;
		editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO | DRAWN_FOOTER);
		editor->saved = 0;
		return 0;
	}
	case 'x':
	{
		if (editor->cursor == NULL) return 0;
		int byte   = editor->cursor_x / 2;
		int nibble = editor->cursor_x % 2;
		editor->cursor->e[byte] -= nibble ? 0x01 : 0x10;
		editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO | DRAWN_FOOTER);
		editor->saved = 0;
		return 0;
	}
	case 'X':
	{
		if (editor->cursor == NULL) return 0;
		int byte   = editor->cursor_x / 2;
		int nibble = editor->cursor_x % 2;
		editor->cursor->e[byte] -= nibble ? 0x04 : 0x40;
		editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO | DRAWN_FOOTER);
		editor->saved = 0;
		return 0;
	}
	case 'a':
		editor_visual_off(editor);
		editor->cursor = list_insert_after(&editor->data, editor->cursor, default_instr);
		editor->cursor_y++;
		editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO | DRAWN_FOOTER);
		editor->saved = 0;
		return 0;
	case 'A':
		editor_visual_off(editor);
		editor->cursor = list_insert_before(&editor->data, editor->cursor, default_instr);
		editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO | DRAWN_FOOTER);
		editor->saved = 0;
		return 0;
	case 'v':
		if (editor->cursor == NULL) return 0;
		if (editor->visual & VISUAL_ON) editor_visual_off(editor);
		else editor_visual_on(editor);
		editor->drawn &= ~DRAWN_HEX;
		return 0;
	case 'c':
	{
		if (editor->cursor == NULL) return 0;
		list_clear(&editor->clipboard);
		ListNode *it1 = (editor->visual & VISUAL_DIR) ? editor->cursor : editor->visual_anchor;
		ListNode *it2 = (editor->visual & VISUAL_DIR) ? editor->visual_anchor : editor->cursor;
		if (!(editor->visual & VISUAL_ON)) it1 = it2 = editor->cursor;
		for (;;)
		{
			list_insert_after(&editor->clipboard, editor->clipboard.back, it1->e);
			if (it1 == it2) break;
			it1 = it1->next;
		}
		editor_visual_off(editor);
		editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO);
		return 0;
	}
	case 'd':
	{
		if (editor->cursor == NULL) return 0;
		list_clear(&editor->clipboard);
		ListNode *it1 = (editor->visual & VISUAL_DIR) ? editor->cursor : editor->visual_anchor;
		ListNode *it2 = (editor->visual & VISUAL_DIR) ? editor->visual_anchor : editor->cursor;
		if (!(editor->visual & VISUAL_ON)) it1 = it2 = editor->cursor;
		int cursor_y_after = (editor->visual & VISUAL_DIR) ? editor->cursor_y : editor->visual_y;
		if (!(editor->visual & VISUAL_ON)) cursor_y_after = editor->cursor_y;
		ListNode *cursor_after = it2->next;
		if (cursor_after == NULL)
		{
			cursor_after = it1->prev;
			cursor_y_after--;
		}
		for (;;)
		{
			// TODO: do a transplant instead of cloning
			list_insert_after(&editor->clipboard, editor->clipboard.back, it1->e);
			ListNode *next = it1->next;
			list_remove(&editor->data, it1);
			if (it1 == it2) break;
			it1 = next;
		}
		editor_visual_off(editor);
		editor->cursor   = cursor_after;
		editor->cursor_y = cursor_y_after;
		editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO | DRAWN_FOOTER);
		editor->saved = 0;
		return 0;
	}
	case 'C':
	{
		editor_visual_off(editor);
		if (editor->clipboard.length == 0) return 0;
		ListNode *it1 = editor->clipboard.front;
		ListNode *it2 = editor->clipboard.back;
		for (;;)
		{
			editor->cursor = list_insert_after(&editor->data, editor->cursor, it1->e);
			editor->cursor_y++;
			if (it1 == it2) break;
			it1 = it1->next;
		}
		editor->drawn &= ~(DRAWN_HEX | DRAWN_INFO | DRAWN_FOOTER);
		editor->saved = 0;
		return 0;
	}
	case 's':
		editor_save(editor, 0);
		editor->drawn &= ~DRAWN_FOOTER;
		return 0;
	case 'S':
		editor_save(editor, 1);
		editor->drawn &= ~DRAWN_FOOTER;
		return 0;
	case 'o':
		editor_visual_off(editor);
		if (editor->saved == 0) editor_save(editor, 0);
		editor_load2(editor);
		return 0;
	case 'O':
		editor_visual_off(editor);
		editor_load2(editor);
		return 0;
	case 'r':
		if (editor->rendering) return 0;
		if (render_options_set(&editor->render_options))
		{
			editor->drawn = 0;
			halfdelay(1);
			return 0;
		}
		editor->render_context = render_start(&editor->data, &editor->render_options);
		if (editor->render_context == NULL)
			editor->rendering_error = 1;
		else
		{
			editor->rendering_error = 0;
			editor->rendering = 1;
		}
		editor->drawn = 0;
		halfdelay(1);
		return 0;
	case 'R':
		if (!editor->rendering) return 0;
		editor_uncurses();
		puts("stopping render thread...");
		render_stop(editor->render_context);
		editor_recurses();
		editor->render_context = NULL;
		editor->rendering = 0;
		editor->drawn &= ~DRAWN_RENDER;
		return 0;
	case '#':
		editor->accidental ^= 1;
		editor->drawn &= ~DRAWN_INFO;
		return 0;
	case '@':
		editor->label_mode ^= 1;
		editor->drawn &= ~DRAWN_INFO;
		return 0;
	case '?':
		editor_uncurses();
		puts("navigation:");
		puts(" vim  | arrow |     keypad");
		puts("------+-------+---------------");
		puts("  k   |   ^   |      PgUp");
		puts("h j l | < v > | Home PgDn End");
		puts("");
		puts("?: display this help message");
		puts("r: start rendering");
		puts("z/Z: increment nibble by 1/4");
		puts("x/X: decrement nibble by 1/4");
		puts("#: toggle accidentals");
		puts("@: change label display mode");
		puts("v: toggle visual mode");
		puts("c/d: copy/cut current selection to internal clipboard");
		puts("C: paste from internal clipboard");
		puts("a/A: add an instruction below/above cursor");
		puts("s/S: save/save as");
		puts("o/O: open new file/open new file without saving");
		puts("q/Q: quit/quit without saving");
		editor_recurses_wait();
		return 0;
	default: return 0;
	}
}

int editor_tick(Editor *editor)
{
	if (editor_validate(editor)) return 1;
	if (editor_redraw(editor)) return 1;
	if (editor_inputs(editor)) return 1;
	return 0;
}

int editor_destroy(Editor *editor)
{
	if (editor->hex)    delwin(editor->hex);
	if (editor->info)   delwin(editor->info);
	if (editor->footer) delwin(editor->footer);
	if (editor->render) delwin(editor->render);
	list_destroy(&editor->data);
	list_destroy(&editor->clipboard);
	endwin();
	return 0;
}
