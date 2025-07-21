#include "config.h"

// This file is intentionally left sparse. It is included to ensure that the
// constants defined in the header are part of the compilation unit and to
// provide a place for future configuration-related functions if needed.

// Add a dummy variable definition to prevent "empty translation unit" warnings
// from some compilers, which can occur if a source file contains no code.
#if defined(__GNUC__) || defined(__clang__)
static const int config_c_dummy_variable __attribute__((unused)) = 0;
#else
static const int config_c_dummy_variable = 0;
#endif
