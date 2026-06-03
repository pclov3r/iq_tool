#define MINIAUDIO_IMPLEMENTATION

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#define MA_NO_DECODING
#define MA_NO_ENCODING
#include "miniaudio.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
