/**
 * @file utilities.c
 * @brief String formatting, unit conversion, and terminal display helpers.
 */

#include "utilities.h"
#include "app_context.h"
#include "log.h"
#include "mem_arena.h"
#include "platform.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void utility_clear_stdin(void) {
  int c;
  while ((c = getchar()) != '\n' && c != EOF)
    ;
}

const char *utility_format_size(long long size_bytes, char *buffer,
                                size_t buffer_size) {
  static const char *error_msg = "(N/A)";
  if (!buffer || buffer_size == 0)
    return error_msg;
  if (size_bytes < 0) {
    snprintf(buffer, buffer_size, "%s", error_msg);
    return buffer;
  }
  double size_d = (double)size_bytes;
  const long long kilo = 1000;
  const long long mega = 1000 * 1000;
  const long long giga = 1000 * 1000 * 1000;
  if (size_bytes < kilo) {
    snprintf(buffer, buffer_size, "%lld B", size_bytes);
  } else if (size_bytes < mega) {
    snprintf(buffer, buffer_size, "%.2f KB", size_d / kilo);
  } else if (size_bytes < giga) {
    snprintf(buffer, buffer_size, "%.2f MB", size_d / mega);
  } else {
    snprintf(buffer, buffer_size, "%.2f GB", size_d / giga);
  }
  return buffer;
}

const char *utility_get_basename_for_parsing(const AppConfig *config,
                                             char *buffer, size_t buffer_size,
                                             MemoryArena *arena) {
  (void)arena;
  if (!config || !config->input.resolved_path || !buffer || buffer_size == 0) {
    return NULL;
  }
  const char *base = platform_get_basename(config->input.resolved_path);
  strncpy(buffer, base, buffer_size - 1);
  buffer[buffer_size - 1] = '\0';
  return buffer;
}

void utility_add_summary_item(InputSummaryInfo *info, const char *label,
                              const char *value_fmt, ...) {
  if (info->count >= APP_MAX_SUMMARY_ITEMS) {
    return;
  }
  SummaryItem *item = &info->items[info->count];
  strncpy(item->label, label, sizeof(item->label) - 1);
  item->label[sizeof(item->label) - 1] = '\0';
  va_list args;
  va_start(args, value_fmt);
  vsnprintf(item->value, sizeof(item->value), value_fmt, args);
  va_end(args);
  item->value[sizeof(item->value) - 1] = '\0';
  info->count++;
}

char *utility_trim_whitespace(char *input_string) {
  if (!input_string)
    return NULL;
  char *end;
  while (isspace((unsigned char)*input_string))
    input_string++;
  if (*input_string == 0) {
    return input_string;
  }
  end = input_string + strlen(input_string) - 1;
  while (end > input_string && isspace((unsigned char)*end))
    end--;
  end[1] = '\0';
  return input_string;
}

void utility_format_duration(double total_seconds, char *buffer,
                             size_t buffer_size) {
  if (!isfinite(total_seconds) || total_seconds < 0) {
    snprintf(buffer, buffer_size, "N/A");
    return;
  }
  if (total_seconds > 0 && total_seconds < 1.0) {
    total_seconds = 1.0;
  }
  int hours = (int)(total_seconds / 3600);
  total_seconds -= hours * 3600;
  int minutes = (int)(total_seconds / 60);
  total_seconds -= minutes * 60;
  int seconds = (int)round(total_seconds);
  if (seconds >= 60) {
    minutes++;
    seconds = 0;
  }
  if (minutes >= 60) {
    hours++;
    minutes = 0;
  }
  snprintf(buffer, buffer_size, "%02d:%02d:%02d", hours, minutes, seconds);
}

bool utility_check_file_exists(const char *full_path) {
  return platform_is_file(full_path);
}

bool utility_prompt_for_overwrite(const char *path_for_messages) {
  fprintf(stderr,
          "\nOutput file %s exists.\nOverwrite? (y/n): ", path_for_messages);
  int response = getchar();
  if (response != '\n' && response != EOF) {
    utility_clear_stdin();
  }
  response = tolower(response);
  if (response != 'y') {
    if (response != '\n' && response != EOF) {
      log_info("Operation cancelled by user.");
    }
    return false;
  }
  return true;
}

bool utility_verify_output_path(const AppConfig *config,
                                const char *out_path_utf8) {
  (void)config;
  if (!out_path_utf8)
    return true;

  if (platform_is_directory(out_path_utf8)) {
    log_error("Output path '%s' is a directory. Aborting.", out_path_utf8);
    return false;
  }

  // Only trigger the interactive overwrite prompt if it's an existing regular
  // file. This allows streaming to /dev/null or FIFOs on Linux without prompts.
  if (platform_is_file(out_path_utf8)) {
    if (!utility_prompt_for_overwrite(out_path_utf8)) {
      return false;
    }
  }

  return true;
}
