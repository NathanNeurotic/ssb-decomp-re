/*
 * ELF entry. Deliberately includes nothing: every library's include path
 * reaches this translation unit, and the game's headers (PR/ultratypes.h)
 * and ps2sdk's (tamtypes.h) cannot share one. See ps2/src/platform/boot.c.
 */
extern int ps2_main(int argc, char *argv[]);

int main(int argc, char *argv[])
{
    return ps2_main(argc, argv);
}
