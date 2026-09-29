/*
 * ps2/ps2_link.x - PS2 values for the N64 linker constants the game uses.
 *
 * Passed to the linker as an implicit script (ldflags in ps2.yaml), next to
 * the decomp's generated symbols/reloc_data_symbols.us.txt (the ll* relocData
 * file IDs and in-file offsets, which are identical on PS2 because the pack
 * keeps the N64 data layout).
 *
 * Counterparts of symbols/linker_constants.txt for the N64 build.
 */

/* ---- Particle banks: served by the asset pack at their N64 ROM addresses
   (ps2/tools/build_assets.py verifies native size == ROM size). ---- */
lEFCommonParticleScriptBankLo  = 0xAC7340;  lEFCommonParticleScriptBankHi  = 0xAC9DE0;
lEFCommonParticleTextureBankLo = 0xAC9DE0;  lEFCommonParticleTextureBankHi = 0xB16C80;
particles_unk0_scb_ROM_START   = 0xB16C80;  particles_unk0_scb_ROM_END     = 0xB17060;
particles_unk0_txb_ROM_START   = 0xB17060;  particles_unk0_txb_ROM_END     = 0xB174A0;
particles_unk1_scb_ROM_START   = 0xB174A0;  particles_unk1_scb_ROM_END     = 0xB176A0;
particles_unk1_txb_ROM_START   = 0xB176A0;  particles_unk1_txb_ROM_END     = 0xB19700;
particles_unk2_scb_ROM_START   = 0xB19700;  particles_unk2_scb_ROM_END     = 0xB19850;
particles_unk2_txb_ROM_START   = 0xB19850;  particles_unk2_txb_ROM_END     = 0xB1BCA0;
lITManagerParticleScriptBankLo = 0xB1BCA0;  lITManagerParticleScriptBankHi = 0xB1BDE0;
lITManagerParticleTextureBankLo = 0xB1BDE0; lITManagerParticleTextureBankHi = 0xB1E640;
lGRPupupuParticleScriptBankLo  = 0xB1E640;  lGRPupupuParticleScriptBankHi  = 0xB1E7E0;
lGRPupupuParticleTextureBankLo = 0xB1E7E0;  lGRPupupuParticleTextureBankHi = 0xB1F960;
lGRHyruleParticleScriptBankLo  = 0xB1F960;  lGRHyruleParticleScriptBankHi  = 0xB1FC80;
lGRHyruleParticleTextureBankLo = 0xB1FC80;  lGRHyruleParticleTextureBankHi = 0xB22980;
lGRYosterParticleScriptBankLo  = 0xB22980;  lGRYosterParticleScriptBankHi  = 0xB22A00;
lGRYosterParticleTextureBankLo = 0xB22A00;  lGRYosterParticleTextureBankHi = 0xB22C30;
lMNTitleParticleScriptBankLo   = 0xB22C30;  lMNTitleParticleScriptBankHi   = 0xB22D40;
lMNTitleParticleTextureBankLo  = 0xB22D40;  lMNTitleParticleTextureBankHi  = 0xB277B0;

/* ---- relocData: the pack re-encodes the table (uncompressed, native
   endianness) at this virtual ROM address. Keep in sync with RELOC_VROM in
   ps2/tools/build_assets.py. ---- */
lLBRelocTableAddr     = 0x20000000;
lLBRelocTableFilesNum = 0x000854;

/* ---- Symbols the N64 build defines as offsets into other objects. ---- */
/* u8 alias of dSYTaskmanFrameCount's low byte: +3 on big-endian N64, +0 here. */
D_8003B6EB_3C2EB = dSYTaskmanFrameCount + 0;
gSYControllerMainButtonTap    = gSYControllerMain + 2;
gSYControllerMainButtonUpdate = gSYControllerMain + 4;

/* Offsets used as plain numbers. */
D_NF_00006010 = 0x06010;
D_NF_00006450 = 0x06450;

/* sys/audio.c refers to its own settings block through two extra names. */
dSYAudioPublicSettings2 = dSYAudioPublicSettings;
dSYAudioPublicSettings3 = dSYAudioPublicSettings;

/* ---- Audio: sequence bank, instrument banks, FGM engine data, served by
   the asset pack at their N64 ROM addresses. ---- */
S1_music_sbk_ROM_START   = 0xB277B0;  S1_music_sbk_ROM_END   = 0xB4E5C0;
B1_sounds1_ctl_ROM_START = 0xB4E5C0;  B1_sounds1_ctl_ROM_END = 0xB54CE0;
B1_sounds2_ctl_ROM_START = 0xC6B650;  B1_sounds2_ctl_ROM_END = 0xC7B1F0;
fgm_unk_ROM_START        = 0xF573D0;  fgm_unk_ROM_END        = 0xF57BF0;
fgm_tbl_ROM_START        = 0xF57BF0;  fgm_tbl_ROM_END        = 0xF5A9C0;
fgm_ucd_ROM_START        = 0xF5A9C0;  fgm_ucd_ROM_END        = 0xF5F4E0;
