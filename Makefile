# Top-level Makefile for IDUN Doom
# Run on the IDUN Pi (Arch Linux ARM):  make && make install

.PHONY: all install clean

all:
	$(MAKE) -C doom

install:
	$(MAKE) -C doom install
	$(MAKE) -C cbm  install
	@echo ""
	@echo "=== IDUN Doom installed ==="
	@echo "Put doom1.wad in ~/doom1.wad on the Pi, then on the C64:"
	@echo "  go doom"

clean:
	$(MAKE) -C doom clean
	$(MAKE) -C cbm  clean
