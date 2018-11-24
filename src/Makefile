CC=gcc
CFLAGS=-I. -lgpiod -Wall
DEPS=libgpiod_pulsein.h
OBJ=libgpiod_pulsein.o

%.o: %.c $(DEPS)
		$(CC) -c -O3 -o $@ $< $(CFLAGS)

libgpiod_pulsein: $(OBJ)
		$(CC) -o $@ $^ $(CFLAGS)
