CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu99
LDLIBS  := -lm

SRC := src/main.c src/model.c src/npz.c src/sha256.c src/debug_dump.c src/struct.c src/chains.c src/pairs.c src/targeted.c src/evaluate.c
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)

spg: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -f $(OBJ) $(DEP) spg

-include $(DEP)

.PHONY: clean
