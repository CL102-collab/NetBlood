// NetBlood adapter: reuse the existing libogg ABI and implementation in audiolib.
// No second copy of libogg is linked into the executable.
#ifndef NETBLOOD_THEORA_OGG_H
#define NETBLOOD_THEORA_OGG_H
#define OV_EXCLUDE_STATIC_CALLBACKS
#include "../../../audiolib/src/minivorbis.h"
#endif
