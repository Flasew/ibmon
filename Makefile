CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=
LIBS ?= -lncurses -lm

.PHONY: all clean

all: ibmon

ib_bw_mon: ibmon.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f ibmon
