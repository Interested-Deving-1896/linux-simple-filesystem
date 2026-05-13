CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=gnu11
LDFLAGS ?=

BINS := simplefs_cli simplefs_test

.PHONY: all clean

all: $(BINS)

simplefs_cli: simplefs_cli.c ../kernel/simplefs.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

simplefs_test: simplefs_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BINS)
