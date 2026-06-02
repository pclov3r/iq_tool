/**
 * @file keyboard_handler.c
 * @brief Implements interactive, non-blocking keyboard input handling.
 */

#include "keyboard_handler.h"
#include "module.h"
#include "signal_handler.h"
#include "log.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#endif

/**
 * @brief Background thread that polls the terminal for keystrokes.
 *
 * @param arg Pointer to the AppContext.
 * @return NULL
 */
static void* keyboard_listener_thread(void* arg) {
    AppContext* app = (AppContext*)arg;
    ModuleContext context = { .config = app->config, .app = app };

#ifndef _WIN32
    // Open the controlling terminal directly (bypasses stdin redirection)
    int tty_fd = open("/dev/tty", O_RDONLY | O_NONBLOCK);
    if (tty_fd < 0) {
        log_warn("Could not open /dev/tty (%s). Interactive keyboard controls disabled.", strerror(errno));
        return NULL;
    }

    // Ignore background terminal signals so headless scripts don't freeze
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);

    // Set terminal to non-blocking raw mode
    struct termios old_term, new_term;
    tcgetattr(tty_fd, &old_term);
    new_term = old_term;
    new_term.c_lflag &= ~(ICANON | ECHO); // Disable buffering and echo
    tcsetattr(tty_fd, TCSANOW, &new_term);
#endif

    while (!is_shutdown_requested()) {
        int key = -1;

#ifdef _WIN32
        // On Windows, _kbhit() already bypasses stdin and reads the console buffer directly
        if (_kbhit()) {
            key = _getch();
        }
#else
        unsigned char c;
        if (read(tty_fd, &c, 1) == 1) {
            key = c;
        }
#endif

        if (key > 0 && key != EOF) {
            log_debug("Keyboard command received: '%c' (0x%02X)", key, key);
            // Broadcast keystroke to active modules
            if (app->module.input_api && app->module.input_api->on_keypress) {
                app->module.input_api->on_keypress(&context, key);
            }
            if (app->module.output_api && app->module.output_api->on_keypress) {
                app->module.output_api->on_keypress(&context, key);
            }
        }

#ifdef _WIN32
        Sleep(50);
#else
        usleep(50 * 1000); // 50ms sleep to keep CPU at 0%
#endif
    }

#ifndef _WIN32
    // Restore terminal to normal mode and close file descriptor
    tcsetattr(tty_fd, TCSANOW, &old_term);
    close(tty_fd);
#endif

    return NULL;
}

void setup_keyboard_handler(AppContext* app) {
    bool wants_keyboard = false;
    if (app->module.input_api && app->module.input_api->on_keypress) wants_keyboard = true;
    if (app->module.output_api && app->module.output_api->on_keypress) wants_keyboard = true;

    if (!wants_keyboard) {
        return; // Don't steal terminal focus or trigger SIGTTOU if no modules care
    }

    pthread_t thread_id;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread_id, &attr, keyboard_listener_thread, app);
    pthread_attr_destroy(&attr);
}
