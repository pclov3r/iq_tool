#include "input_manager.h"
#include <string.h>

// Include the headers for all concrete input source implementations
#include "input_wav.h"
#if defined(WITH_SDRPLAY)
#include "input_sdrplay.h"
#endif
#if defined(WITH_HACKRF)
#include "input_hackrf.h"
#endif

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

InputSourceOps* get_input_ops_by_name(const char* name) {
    if (!name) {
        return NULL;
    }

    if (strcasecmp(name, "wav") == 0) {
        return get_wav_input_ops();
    }
#if defined(WITH_SDRPLAY)
    else if (strcasecmp(name, "sdrplay") == 0) {
        return get_sdrplay_input_ops();
    }
#endif
#if defined(WITH_HACKRF)
    else if (strcasecmp(name, "hackrf") == 0) {
        return get_hackrf_input_ops();
    }
#endif

    return NULL; // Name not recognized
}
