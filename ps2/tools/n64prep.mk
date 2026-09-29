# ps2/tools/n64prep.mk
#
# Stage 1 of the PS2 asset pipeline. Runs the *existing* N64 decomp
# extraction pipeline (unchanged top-level Makefile) far enough to produce
# every generated source the PS2 port consumes, WITHOUT building the N64 ROM
# (so IDO / the IRIX4 frontend are not required for the PS2 port).
#
#   make -f ps2/tools/n64prep.mk ps2-prep VERSION=us
#
# Outputs (all under build/<v>/, never committed):
#   - src/relocData/<Name>/*.inc.c        typed Vtx/Gfx/palette/texture bytes
#   - src/relocData/<id>_<Name>.c         generated master .c (manifest/spritelist files)
#   - include/reloc_data.<v>.h            ll* file-offset constants used by game code
#   - src/credits/*.encoded               staff roll data
#   - src/particles/*.inc.c               particle bank textures
#   - src/audio/*                         sequences / instrument banks / fgm blobs
#   - ps2_reloc_masters.txt               "<fid> <master .c path>" per relocData file
#
# The PS2 build never runs IDO; it recompiles these sources natively with the
# PS2Build EE toolchain (see ps2/tools/build_assets.py).

RELOC_DATA := 1
include Makefile

PS2_PREP_MASTERS := $(BUILD_DIR)/ps2_reloc_masters.txt

# Same as the top-level stamp rules minus the PNG preview step (previews are
# only for humans and take most of the extraction time).
$(BUILD_DIR)/src/relocData/.build/.extract-%.stamp: assets/$(VERSION)/relocData/%.vpk0.bin
	@mkdir -p $(@D)
	$(V)$(PYTHON) tools/relocSpriteTool.py extract $* --version $(VERSION) >/dev/null
	$(V)$(PYTHON) tools/extractRelocInc.py $* --version $(VERSION) >/dev/null
	@touch $@

$(BUILD_DIR)/src/relocData/.build/.extract-%.stamp: assets/$(VERSION)/relocData/%.bin
	@mkdir -p $(@D)
	$(V)$(PYTHON) tools/relocSpriteTool.py extract $* --version $(VERSION) >/dev/null
	$(V)$(PYTHON) tools/extractRelocInc.py $* --version $(VERSION) >/dev/null
	@touch $@

PS2_RELOC_MASTERS := $(foreach f,$(RELOC_C_FILES),$(RELOC_MASTER_$(f)))
PS2_CREDITS := $(foreach n,staff titles info companies,$(BUILD_DIR)/src/credits/$(n).credits.encoded)
PS2_PARTICLES := $(foreach b,$(PARTICLE_BANKS),$(BUILD_DIR)/src/particles/$(b).extract-stamp)

.PHONY: ps2-prep ps2-prep-audio
ps2-prep: $(RELOCDATA_EXTRACT_STAMPS) include/reloc_data.$(VERSION).h \
          $(PS2_RELOC_MASTERS) $(PS2_CREDITS) $(PS2_PARTICLES) ps2-prep-audio
	$(file >$(PS2_PREP_MASTERS),$(foreach f,$(RELOC_C_FILES),$(f) $(RELOC_MASTER_$(f))$(newline)))
	@echo "ps2-prep: $(words $(RELOC_C_FILES)) relocData files ready; manifest -> $(PS2_PREP_MASTERS)"

ps2-prep-audio: $(MUSIC_SBK_C) $(MUSIC_SEQ_INCS) $(SOUND_CTL_C) $(SOUND_TBL_BINS)

define newline


endef
