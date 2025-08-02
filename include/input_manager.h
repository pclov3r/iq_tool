// input_manager.h
#ifndef INPUT_MANAGER_H_
#define INPUT_MANAGER_H_

#include "input_source.h"

/**
 * @brief Gets the appropriate InputSourceOps implementation based on a name.
 *
 * This function acts as a factory, returning the set of function pointers
 * for the requested input source (e.g., "wav", "sdrplay").
 *
 * @param name The name of the input source, typically from the --input argument.
 * @return A pointer to the corresponding static InputSourceOps struct, or NULL if
 *         the name is not recognized or the module is not compiled in.
 */
InputSourceOps* get_input_ops_by_name(const char* name);

#endif // INPUT_MANAGER_H_
