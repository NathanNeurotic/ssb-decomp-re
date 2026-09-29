#ifndef _FTCHARDATA_H_
#define _FTCHARDATA_H_

// Per-character FTStatusDesc/FTMotionDesc table declarations. They live here
// (included at the end of fttypes.h, after the struct definitions) because
// an extern array of a not-yet-complete element type is invalid C; IDO
// accepted it inside the per-character headers, GCC does not.

// ft/ftchar/ftboss/ftboss.h
extern FTStatusDesc dFTBossSpecialStatusDescs[/* */];
// ft/ftchar/ftcaptain/ftcaptain.h
extern FTStatusDesc dFTCaptainSpecialStatusDescs[/* */];
// ft/ftchar/ftdonkey/ftdonkey.h
extern FTStatusDesc dFTDonkeySpecialStatusDescs[/* */];
// ft/ftchar/ftfox/ftfox.h
extern FTStatusDesc dFTFoxSpecialStatusDescs[/* */];
// ft/ftchar/ftkirby/ftkirby.h
extern FTStatusDesc dFTKirbySpecialStatusDescs[/* */];
// ft/ftchar/ftlink/ftlink.h
extern FTStatusDesc dFTLinkSpecialStatusDescs[/* */];
// ft/ftchar/ftluigi/ftluigi.h
extern FTStatusDesc dFTLuigiSpecialStatusDescs[/* */];
// ft/ftchar/ftmario/ftmario.h
extern FTStatusDesc dFTMarioSpecialStatusDescs[/* */];
extern FTMotionDesc dFTMarioMotionDescs[/* */];
// ft/ftchar/ftness/ftness.h
extern FTStatusDesc dFTNessSpecialStatusDescs[/* */];
// ft/ftchar/ftpikachu/ftpikachu.h
extern FTStatusDesc dFTPikachuSpecialStatusDescs[/* */];
// ft/ftchar/ftpurin/ftpurin.h
extern FTStatusDesc dFTPurinSpecialStatusDescs[/* */];
// ft/ftchar/ftsamus/ftsamus.h
extern FTStatusDesc dFTSamusSpecialStatusDescs[/* */];
// ft/ftchar/ftyoshi/ftyoshi.h
extern FTStatusDesc dFTYoshiSpecialStatusDescs[/* */];

#endif
