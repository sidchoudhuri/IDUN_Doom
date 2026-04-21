# Makefile for doom-idun (Pi Zero 2 W, Arch Linux ARM)
# Clones RAD Doom for the Doom engine source, then compiles with our
# doomgeneric_idun.c replacing doomgeneric_rad.c.

CC      = gcc
CFLAGS  = -O2 -Wall -Wno-unused-result \
          -Dlinux -D_GNU_SOURCE \
          -I. -I$(DOOM_SRC)
LDFLAGS = -lm

BINARY  = doom-idun
INSTALL = /usr/local/bin/$(BINARY)

RAD_REPO  = https://github.com/frntc/RAD-Doom.git
RAD_DIR   = ../rad-doom-src
DOOM_SRC  = $(RAD_DIR)/Source/Doom

# All Doom engine .c files except the RAD-specific ones
DOOM_SRCS = $(filter-out \
    $(DOOM_SRC)/doomgeneric_rad.c \
    $(DOOM_SRC)/i_sdlmusic.c \
    $(DOOM_SRC)/i_sdlsound.c \
    $(DOOM_SRC)/icon.c, \
    $(wildcard $(DOOM_SRC)/*.c))

# Our integration file + the two support files from RAD we still need
OUR_SRCS = doomgeneric_idun.c doomgeneric.c

SRCS = $(DOOM_SRCS) $(OUR_SRCS)
OBJS = $(notdir $(SRCS:.c=.o))

.PHONY: all clean install fetch

all: fetch $(BINARY)

# Clone RAD Doom to get the engine source (we only use Source/Doom/)
fetch:
	@if [ ! -d "$(RAD_DIR)" ]; then \
	    echo "Cloning RAD Doom for engine source..."; \
	    git clone --depth=1 $(RAD_REPO) $(RAD_DIR); \
	fi

# Copy doomgeneric.c from RAD (it's mostly malloc wrappers)
doomgeneric.c: fetch
	@cp $(DOOM_SRC)/doomgeneric.c .
	@# Replace Rmalloc with plain malloc for Linux
	@sed -i 's/Rmalloc/malloc/g' doomgeneric.c
	@sed -i 's|#include.*mempool_c\.h.*||g' doomgeneric.c
	@sed -i '1s/^/#include <stdlib.h>\n/' doomgeneric.c

# Pattern rules: compile from either our dir or DOOM_SRC
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(DOOM_SRC)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BINARY): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

install: $(BINARY)
	install -m 755 $(BINARY) $(INSTALL)

clean:
	rm -f $(OBJS) $(BINARY) doomgeneric.c
