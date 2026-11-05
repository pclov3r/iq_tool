/**
 * @file utility_threads.h
 * @brief Declares the implementation functions for asynchronous service
 * threads.
 *
 * These threads run in the background to support the main process_chain but are
 * not part of the direct data flow.
 */

#ifndef UTILITY_THREADS_H_
#define UTILITY_THREADS_H_

struct AppContext;
struct WaitEvent;

/**
 * @struct SdrInitWatchdogContext
 * @brief Context passed to the SDR initialization watchdog thread.
 */
typedef struct SdrInitWatchdogContext {
  const char *module_name; ///< Name of the input module being initialized.
  struct WaitEvent *
      completion_event; ///< Event signaled when initialization completes.
} SdrInitWatchdogContext;

/**
 * @brief Watchdog thread that forces application exit if SDR initialization
 * hangs.
 * @param arg A pointer to the SdrInitWatchdogContext struct.
 * @return NULL.
 */
void *sdr_init_watchdog_thread(void *arg);

/**
 * @brief Sets up and spawns the input watchdog thread if in live streaming
 * mode.
 * @param app Pointer to the global application context.
 */
void setup_input_watchdog(struct AppContext *app);

#endif // UTILITY_THREADS_H_
