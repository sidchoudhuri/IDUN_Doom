# Makefile for C64-side IDUN Doom app
# Assembles doom.app.s with ACME and installs to the IDUN sys/ directory.

ACME    = acme
SRCDIR  = .
SYSDIR  = $(HOME)/idun-cartridge/cbm/sys
APPNAME = doom
OUTDIR  = $(HOME)/idun-cartridge/cbm

.PHONY: all clean install

all: $(APPNAME).app

$(APPNAME).app: doom.app.s doom.app.d/main.lua
	$(ACME) --cpu 6502 \
	        --format cbm \
	        -I $(SYSDIR) \
	        -o $@ \
	        doom.app.s

install: $(APPNAME).app
	@mkdir -p $(OUTDIR)/$(APPNAME).app.d
	install -m 644 $(APPNAME).app     $(OUTDIR)/$(APPNAME).app
	install -m 644 doom.app.d/main.lua $(OUTDIR)/$(APPNAME).app.d/main.lua
	@echo "Installed $(APPNAME).app – run 'make' in idun-cartridge/cbm to rebuild sys"

clean:
	rm -f $(APPNAME).app
