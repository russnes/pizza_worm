ifeq ($(strip $(PVSNESLIB_HOME)),)
$(error "Please create an environment variable PVSNESLIB_HOME by following this guide: https://github.com/alekmaul/pvsneslib/wiki/Installation")
endif

# list in AUDIOFILES all your .it files in the right order. It will build to generate soundbank file
AUDIOFILES := res/pizza_music.it
# then define the path to generate soundbank data
export SOUNDBANK := res/soundbank
# soundbank conversion flags
SMCONVFLAGS := -s -o $(SOUNDBANK) -V -b 5

include ${PVSNESLIB_HOME}/devkitsnes/snes_rules

.PHONY: bitmaps all musics checkram

#---------------------------------------------------------------------------------
export ROMNAME := pizzaworm

all: bitmaps musics $(ROMNAME).sfc checkram

clean: cleanBuildRes cleanRom cleanGfx cleanAudio

# wla-link does not notice when the C .bss runs into pvsneslib's RAM; this does (see check_ram.py)
checkram: $(ROMNAME).sfc
	@python3 check_ram.py $(ROMNAME).sym

#---------------------------------------------------------------------------------
sprites.bmp sprite_tiles.h body_dot.inc: gen_sprites.py
	@echo generating sprite sheet ...
	python3 gen_sprites.py

tiles1.bmp tiles_charmap.h: gen_tiles.py
	@echo generating tileset ...
	python3 gen_tiles.py

# the unlit version of the neon sign on the title art (see gen_sign_off.py)
sign_off.bmp sign_off.h: gen_sign_off.py splash.bmp
	@echo generating unlit neon sign ...
	python3 gen_sign_off.py

sign_off.pic: sign_off.bmp
	@echo convert bitmap ... $(notdir $@)
	$(GFXCONV) -s 8 -o 16 -u 16 -t bmp -i $<

trig_table.h: gen_trig.py
	@echo generating trig table ...
	python3 gen_trig.py

sprites.pic: sprites.bmp
	@echo convert bitmap ... $(notdir $@)
	$(GFXCONV) -s 16 -o 16 -u 16 -t bmp -i $<

tiles1.pic: tiles1.bmp
	@echo convert bitmap ... $(notdir $@)
	$(GFXCONV) -s 8 -o 16 -u 16 -t bmp -i $<

splash.pic splash.map splash.pal: splash.bmp
	@echo convert splash bitmap ... $(notdir $@)
	$(GFXCONV) -s 8 -o 16 -u 16 -p -m -R -t bmp -i $<

bitmaps : sprites.pic tiles1.pic splash.pic sign_off.pic trig_table.h sprite_tiles.h sign_off.h body_dot.inc tiles_charmap.h

# data.asm pulls these in with .include / .incbin, so rebuild it when they change
data.obj: body_dot.inc canvas.inc sprites.pic tiles1.pic splash.pic sign_off.pic

musics: $(SOUNDBANK).asm
