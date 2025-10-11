#include <stdio.h>
#include <signal.h>
#include "editor.h"

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	Editor editor;
	editor_create(&editor);
	editor_init_display(&editor);
	if (argc == 2)
	{
		editor_load(&editor, argv[1]);
	}
	while (!editor_tick(&editor));
	editor_destroy(&editor);
	return 0;
}
