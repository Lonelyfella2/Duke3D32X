# ===========================================================================
# Duke Nukem 3D 32X - build
#
#   ./setup.sh          (once per sandbox start: toolchain + PicoDrive core)
#   make assets         assets/ + DUKE3D.GRP -> gen/ (fonts, title screen, game data)
#   make data           convert DUKE3D.GRP -> gen/gamedata_{be,le}.bin
#   make                rom/duke3d32x.32x  (4 MiB)
#   make oracle         desktop render oracle (build/oracle)
#   make check          static ROM checks + PicoDrive scripted tests
# ===========================================================================
GENDEV  ?= /opt/toolchains/sega
TARGET  := duke3d32x
BUILD   := build
ROM     := rom/$(TARGET).32x
GRP     ?= $(shell python3 tools/find_grp.py)

SHPREFIX := $(GENDEV)/sh-elf/bin/sh-elf-
MDPREFIX := $(GENDEV)/m68k-elf/bin/m68k-elf-
CC    := $(SHPREFIX)gcc
AS    := $(SHPREFIX)as
OBJC  := $(SHPREFIX)objcopy
SIZE  := $(SHPREFIX)size
MDAS  := $(MDPREFIX)as
MDLD  := $(MDPREFIX)ld
MDOBJC:= $(MDPREFIX)objcopy
HOSTCC ?= gcc
GCCVER := 12.1.0

INC     := -Isrc/core -Isrc/game -Isrc/platform/32x -Igen
COMMON  := -std=gnu11 -fwrapv -funsigned-char -Wall -Wno-unused-function -Wno-unused-variable
SHFLAGS := -c -m2 -mb -O2 -fomit-frame-pointer -ffunction-sections -fdata-sections \
           -fno-common -D__32X__ -DMARS $(COMMON) $(INC)
LDFLAGS := -T src/platform/32x/mars.ld -nostdlib --specs=nosys.specs -Wl,--gc-sections \
           -Wl,-Map=$(BUILD)/$(TARGET).map
LIBS    := -L$(GENDEV)/sh-elf/sh-elf/lib -L$(GENDEV)/sh-elf/lib/gcc/sh-elf/$(GCCVER) -lc -lgcc -lnosys

CORE    := engine build_engine lz4 render_common render_m render_s
GAME    := $(patsubst src/game/%.c,%,$(wildcard src/game/*.c))
PLAT    := main32x hw32x sound32x
SHOBJS  := $(BUILD)/crt0.o $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE) $(GAME) $(PLAT))) $(BUILD)/gamedata.o

.PHONY: all assets data oracle hostgame check clean
all: $(ROM)

$(BUILD) rom gen out:
	mkdir -p $@

# everything converted from assets/ (and the GRP): the two fonts, the
# pictures (3D Realms, title + logo, level transition; stored in the data
# blob with their own palettes) and the game data
ASSET_HDRS := gen/font8x8.h gen/dukefont.h gen/tormentfont.h
assets: $(ASSET_HDRS) data

PICTURES := assets/title.png assets/title_logo.png assets/title_32x.png assets/transition.png assets/realms.png
data gen/gamedata_be.bin gen/gamedata_le.bin gen/con_defs.h: tools/convert.py $(PICTURES) assets/dukefont.png assets/tormentfont.png | gen
	python3 tools/convert.py --grp "$(GRP)" --out gen \
	    --title assets/title.png --title-logo assets/title_logo.png --title-32x assets/title_32x.png --title-font assets/dukefont.png \
	    --transition assets/transition.png --transition-font assets/tormentfont.png --realms assets/realms.png

# in-game text font (assets/font.png, 16x6 cells of 9x8, ASCII 32..127)
gen/font8x8.h: assets/font.png tools/mkfont.py | gen
	python3 tools/mkfont.py assets/font.png $@

# big fonts: Duke's red one (title, pause screen) and Tormentstein 3D (top
# messages, ENTERING / LEVEL COMPLETED, skill and level menu)
gen/dukefont.h: assets/dukefont.png tools/mkbigfont.py | gen
	python3 tools/mkbigfont.py --prefix dfont assets/dukefont.png $@
gen/tormentfont.h: assets/tormentfont.png tools/mkbigfont.py | gen
	python3 tools/mkbigfont.py --prefix tfont assets/tormentfont.png $@

# ---- 68000 side ----
$(BUILD)/md.o: src/platform/32x/md/crt0.s | $(BUILD)
	$(MDAS) -m68000 --register-prefix-optional $< -o $@
$(BUILD)/md.elf: $(BUILD)/md.o
	$(MDLD) -T $(GENDEV)/ldscripts/mars-md.ld -nostdlib $< -o $@
$(BUILD)/md.bin: $(BUILD)/md.elf
	$(MDOBJC) -O binary $< $@

# ---- SH-2 side ----
$(BUILD)/crt0.o: src/platform/32x/crt0.s $(BUILD)/md.bin
	$(AS) --big $< -o $@
$(BUILD)/gamedata.o: src/platform/32x/gamedata.s gen/gamedata_be.bin | $(BUILD)
	$(AS) --big $< -o $@
$(BUILD)/%.o: src/core/%.c src/core/*.h gen/con_defs.h | $(BUILD)
	$(CC) $(SHFLAGS) $< -o $@
$(BUILD)/render_m.o $(BUILD)/render_s.o: src/core/render_impl.c
$(BUILD)/%.o: src/game/%.c src/game/*.h src/core/*.h gen/con_defs.h $(ASSET_HDRS) | $(BUILD)
	$(CC) $(SHFLAGS) $< -o $@
$(BUILD)/%.o: src/platform/32x/%.c src/platform/32x/*.h src/core/*.h | $(BUILD)
	$(CC) $(SHFLAGS) $< -o $@

$(BUILD)/$(TARGET).elf: $(SHOBJS)
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@
	@$(SIZE) $@
	@python3 tools/memcheck.py $(BUILD)/$(TARGET).map

$(ROM): $(BUILD)/$(TARGET).elf | rom
	$(OBJC) -O binary $< $(BUILD)/$(TARGET).raw
	cp $(BUILD)/$(TARGET).raw $@
	python3 tools/romfix.py $@ --gran 4194304

# ---- desktop game runner (same core+game code, little-endian blob) ----
hostgame: $(BUILD)/hostgame
$(BUILD)/hostgame: $(wildcard src/core/*.c src/core/*.h src/game/*.c src/game/*.h src/platform/host/hostgame.c) gen/con_defs.h $(ASSET_HDRS) | $(BUILD)
	$(HOSTCC) -O1 -g -DHOSTGAME $(COMMON) -Isrc/core -Isrc/game -Igen $(addprefix src/core/,$(addsuffix .c,$(CORE))) \
	    src/game/*.c src/platform/host/hostgame.c -o $@

# ---- desktop oracle (same core/game code, little-endian blob) ----
HOSTFLAGS := -O2 -g $(COMMON) -Isrc/core -Isrc/game -Igen
oracle: $(BUILD)/oracle
$(BUILD)/oracle: $(wildcard src/core/*.c src/core/*.h src/platform/host/*.c) gen/con_defs.h | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) $(addprefix src/core/,$(addsuffix .c,$(CORE))) src/platform/host/oracle.c -o $@

check: $(ROM)
	python3 tests/verify_rom.py $(ROM) $(BUILD)/$(TARGET).elf --title "DUKE NUKEM 3D" \
	    --marker "PRESS START" --marker "QUIT TO TITLE" --marker "HOLLYWOOD HOLOCAUST"
	python3 tests/run_tests.py

clean:
	rm -rf $(BUILD) rom
