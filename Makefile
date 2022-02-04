CC = gcc
CFLAGS = -W -Wall -Wextra -Wshadow -std=c99 -D_FORTIFY_SOURCE=2 -O3 -g

LD = gcc
LDFLAGS = -pie
LIBS = -lpthread

SRC = $(wildcard *.c)
MAIN_SRC = $(shell grep -lE $$'^int[[:space:]]+main[[:space:]]*\\\x28' $(SRC))
COMMON_SRC = $(filter-out $(MAIN_SRC),$(SRC))
COMMON_OBJ = $(COMMON_SRC:%.c=%.o)
OBJ = $(SRC:%.c=%.o)
BIN = $(MAIN_SRC:%.c=%.exe)

.PHONY: all clean

.SECONDARY: $(OBJ)

all: $(BIN)

%.exe: %.o
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

clean:
	rm -f $(OBJ) $(BIN)
