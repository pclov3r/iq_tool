/**
 * @file module_registry.c
 * @brief Central module registry maintaining compiled input, output, and DSP
 * modules.
 */

#include "module_registry.h"
#include "app_context.h"
#include "log.h"
#include "mem_arena.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Private struct and forward declarations ---
typedef struct {
  const char *active_input;
  const char *active_output;
} ModuleWarningContext;

static int inactive_option_warning_cb(struct argparse *self,
                                      const struct argparse_option *opt);

// The master list is built at startup time via __attribute__((constructor))
#define MAX_MODULES 128
static Module all_modules[MAX_MODULES];
static int num_all_modules = 0;

// --- Registry Maintenance ---

void module_registry_add(const Module *m) {
  if (m && num_all_modules < MAX_MODULES) {
    memcpy(&all_modules[num_all_modules], m, sizeof(Module));
    num_all_modules++;
  } else {
    fprintf(stderr, "FATAL: Module registry exceeded MAX_MODULES\n");
    exit(1);
  }
}

static const Module *_find_module_by_name_and_type(const char *name,
                                                   ModuleType type) {
  if (!name) {
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

// --- Public Module Getters ---

const Module *module_get(const char *name, ModuleType type) {
  return _find_module_by_name_and_type(name, type);
}

const struct DspModuleInterface *get_dsp_module(const char *name) {
  const Module *module = module_get(name, MODULE_TYPE_DSP);
  if (module)
    return (const struct DspModuleInterface *)module->api;
  return NULL;
}

// --- Public Queries & Actions ---

const Module *module_get_all(int *count) {
  *count = num_all_modules;
  return all_modules;
}

// --- CLI Options Population ---

void module_populate_cli_options(struct argparse_option *dest_buffer,
                                 int *total_opts_ptr, int max_opts,
                                 const char *active_input_type,
                                 const char *active_output_type,
                                 struct MemoryArena *arena) {

  for (int i = 0; i < num_all_modules; ++i) {
    const struct argparse_option *(*get_opts_fn)(int *) =
        all_modules[i].get_cli_options;
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

// --- Private Helper Functions ---

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
