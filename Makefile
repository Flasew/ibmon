CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=
LIBS ?= -lncurses -lm

.PHONY: all clean

all: ib_bw_mon

ib_bw_mon: ib_bw_mon.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f ib_bw_mon
