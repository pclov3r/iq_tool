/**
 * @file keyboard_handler.h
 * @brief Provides interactive, non-blocking keyboard input handling.
 */

#ifndef KEYBOARD_HANDLER_H_
#define KEYBOARD_HANDLER_H_

struct AppContext;

/**
 * @brief Initializes the background keyboard listener thread.
 *
 * Sets the terminal to raw/cbreak mode to capture keystrokes immediately
 * without waiting for the Enter key. Valid keystrokes are then broadcast
 * to the active input and output modules.
 *
 * @param app Pointer to the global application context.
 */
void setup_keyboard_handler(struct AppContext *app);

#endif // KEYBOARD_HANDLER_H_
