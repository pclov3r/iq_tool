/**
 * @file module_registry.c
 */

#include "module_registry.h"
#include "app_context.h"
#include "log.h"
#include "mem_arena.h"
#include "module_defaults.h"
#include <stdlib.h>
#include <string.h>


#include <stdio.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

// --- Private struct and forward declarations ---
typedef struct {
  const char *active_input;
  const char *active_output;
} ModuleWarningContext;

static int inactive_option_warning_cb(struct argparse *self,
                                      const struct argparse_option *opt);

// The master list is now built at startup time via __attribute__((constructor))
#define MAX_MODULES 128
static Module all_modules[MAX_MODULES];
static int num_all_modules = 0;

void module_registry_add(const Module *m) {
  if (m && num_all_modules < MAX_MODULES) {
    memcpy(&all_modules[num_all_modules], m, sizeof(Module));
    num_all_modules++;
  } else {
    fprintf(stderr, "FATAL: Module registry exceeded MAX_MODULES\n");
    exit(1);
  }
}

static void initialize_modules_list(MemoryArena *arena) {
    // No-op. Modules register themselves before main().
    (void)arena;
}


static const Module *_find_module_by_name_and_type(const char *name,
                                                   ModuleType type,
                                                   MemoryArena *arena) {
  initialize_modules_list(arena); // Ensure the list is ready
  if (!name || !all_modules) {
    return NULL;
  }
  for (int i = 0; i < num_all_modules; ++i) {
    if (all_modules[i].type == type &&
        strcasecmp(name, all_modules[i].name) == 0) {
      return &all_modules[i];
    }
  }
  return NULL; // Not found
}

/**
 * @brief Iterates through all registered modules and applies their default
 * settings.
 */
void module_apply_defaults(AppConfig *config, MemoryArena *arena) {
  initialize_modules_list(arena); // Ensure the list is ready
  if (!all_modules)
    return;

  for (int i = 0; i < num_all_modules; ++i) {
    if (all_modules[i].set_default_config) {
      all_modules[i].set_default_config(config);
    }
  }
}

const Module *module_get_all(int *count, MemoryArena *arena) {
  initialize_modules_list(arena); // Ensure the list is ready
  *count = num_all_modules;
  return all_modules;
}

bool module_is_live_source(const char *name, MemoryArena *arena) {
  const Module *mod =
      _find_module_by_name_and_type(name, MODULE_TYPE_INPUT, arena);
  return (mod != NULL &&
          mod->process_chain_mode == PROCESS_CHAIN_MODE_ASYNCHRONOUS_PUSH);
}

void module_populate_cli_options(struct argparse_option *dest_buffer,
                                 int *total_opts_ptr, int max_opts,
                                 const char *active_input_type,
                                 const char *active_output_type,
                                 struct MemoryArena *arena) {
  initialize_modules_list(arena);
  if (!all_modules)
    return;

  for (int i = 0; i < num_all_modules; ++i) {
    const struct argparse_option *(*get_opts_fn)(int *) =
        all_modules[i].get_cli_options;
    if (!get_opts_fn && all_modules[i].type == MODULE_TYPE_DSP) {
      const DspModuleInterface *dsp =
          (const DspModuleInterface *)all_modules[i].api;
      if (dsp)
        get_opts_fn = dsp->get_cli_options;
    }

    if (get_opts_fn) {
      int count = 0;
      const struct argparse_option *opts = get_opts_fn(&count);
      if (opts && count > 0) {
        if (*total_opts_ptr + count > max_opts) {
          log_fatal("Internal error: Exceeded maximum number of CLI options.");
          return;
        }

        memcpy(&dest_buffer[*total_opts_ptr], opts,
               count * sizeof(struct argparse_option));

        // Allocate our warning context once
        ModuleWarningContext *warning_context =
            (ModuleWarningContext *)mem_arena_alloc(
                arena, sizeof(ModuleWarningContext), true);
        if (warning_context) {
          warning_context->active_input = active_input_type;
          warning_context->active_output = active_output_type;
        }

        bool is_inactive_input =
            (active_input_type && all_modules[i].type == MODULE_TYPE_INPUT &&
             strcasecmp(all_modules[i].name, active_input_type) != 0);
        bool is_inactive_output =
            (active_output_type && all_modules[i].type == MODULE_TYPE_OUTPUT &&
             strcasecmp(all_modules[i].name, active_output_type) != 0);
        // DSP modules are always active in the parser because their flags ARE
        // how you activate them.

        if (is_inactive_input || is_inactive_output) {
          for (int j = 0; j < count; j++) {
            struct argparse_option *opt = &dest_buffer[*total_opts_ptr + j];
            if (opt->type != ARGPARSE_OPT_GROUP) {
              opt->callback = inactive_option_warning_cb;
              opt->data = (intptr_t)warning_context;
            }
          }
        }
        *total_opts_ptr += count;
      }
    }
  }
}

const Module *module_get(const char *name, ModuleType type,
                         MemoryArena *arena) {
  return _find_module_by_name_and_type(name, type, arena);
}

const struct DspModuleInterface *get_dsp_module(const char *name,
                                                struct MemoryArena *arena) {
  const Module *mod = module_get(name, MODULE_TYPE_DSP, arena);
  if (mod)
    return (const struct DspModuleInterface *)mod->api;
  return NULL;
}

// --- Private Helper Functions ---

// Callback triggered when a user provides a flag for a module that isn't
// currently active.
static int inactive_option_warning_cb(struct argparse *self,
                                      const struct argparse_option *opt) {
  if (opt->type != ARGPARSE_OPT_GROUP && opt->data != 0) {
    ModuleWarningContext *context = (ModuleWarningContext *)opt->data;

    const char *user_value = self->optvalue;

    if (user_value) {
      log_warn("Ignoring '--%s %s' because it does not apply to the selected "
               "input ('%s') or output ('%s') module.",
               opt->long_name, user_value,
               context->active_input ? context->active_input : "unknown",
               context->active_output ? context->active_output : "unknown");
    } else {
      log_warn("Ignoring '--%s' because it does not apply to the selected "
               "input ('%s') or output ('%s') module.",
               opt->long_name,
               context->active_input ? context->active_input : "unknown",
               context->active_output ? context->active_output : "unknown");
    }
  }
  return 0;
}
