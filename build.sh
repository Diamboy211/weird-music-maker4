#!/bin/sh
CC=gcc
CFLAGS="-g -Wall -Wextra -pedantic -std=gnu11 -fsanitize=address,undefined -fno-omit-frame-pointer $(ncurses6-config --cflags)"
LIBS="$(ncurses6-config --libs) -lm -pthread"

${CC} ${CFLAGS} -c dbs4.c
${CC} ${CFLAGS} -c list.c
${CC} ${CFLAGS} -c editor.c
${CC} ${CFLAGS} -c instr.c
${CC} ${CFLAGS} -c render.c
${CC} ${CFLAGS} -c render_internal.c
${CC} ${CFLAGS} -c helper.c
${CC} ${CFLAGS} -o main dbs4.o list.o editor.o instr.o render.o render_internal.o helper.o ${LIBS}
