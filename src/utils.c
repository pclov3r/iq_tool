#include "utils.h"
#include "log.h"
#include "mem_arena.h"
#include "app_context.h"
#include "signal_handler.h"
#include "ring_buffer.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#define strcasecmp _stricmp
#else
#include <libgen.h>
#include <strings.h>
#include <unistd.h>
#endif

double utils_get_time(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&count)) {
        return (double)count.QuadPart / (double)freq.QuadPart;
    }
    // Fallback to a lower-resolution timer if QPC fails
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    }
    // Fallback for systems without clock_gettime
    return (double)time(NULL);
#endif
}

void utils_clear_stdin(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

const char* utils_format_size(long long size_bytes, char* buffer, size_t buffer_size) {
    static const char* error_msg = "(N/A)";
    if (!buffer || buffer_size == 0) return error_msg;
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

const char* get_basename_for_parsing(const AppConfig *config, char* buffer, size_t buffer_size, MemoryArena* arena) {
#ifdef _WIN32
    (void)arena; // arena is unused on Windows, this silences the warning.
    if (config->input.effective_path_w[0] != L'\0') {
        const wchar_t* base_w = PathFindFileNameW(config->input.effective_path_w);
        if (WideCharToMultiByte(CP_UTF8, 0, base_w, -1, buffer, buffer_size, NULL, NULL) > 0) {
            return buffer;
        }
    }
#else
    if (config->input.effective_path) {
        size_t len = strlen(config->input.effective_path) + 1;
        char* temp_copy = (char*)mem_arena_alloc(arena, len, false);
        if (temp_copy) {
            strcpy(temp_copy, config->input.effective_path);
            char* base = basename(temp_copy);
            strncpy(buffer, base, buffer_size - 1);
            buffer[buffer_size - 1] = '\0';
            return buffer;
        }
    }
#endif
    return NULL;
}

void add_summary_item(InputSummaryInfo* info, const char* label, const char* value_fmt, ...) {
    if (info->count >= MAX_SUMMARY_ITEMS) {
        return;
    }
    SummaryItem* item = &info->items[info->count];
    strncpy(item->label, label, sizeof(item->label) - 1);
    item->label[sizeof(item->label) - 1] = '\0';
    va_list args;
    va_start(args, value_fmt);
    vsnprintf(item->value, sizeof(item->value), value_fmt, args);
    va_end(args);
    item->value[sizeof(item->value) - 1] = '\0';
    info->count++;
}

char* utils_trim_whitespace(char* str) {
    if (!str) return NULL;
    char* end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) {
        return str;
    }
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

void utils_format_duration(double total_seconds, char* buffer, size_t buffer_size) {
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
    if (seconds >= 60) { minutes++; seconds = 0; }
    if (minutes >= 60) { hours++; minutes = 0; }
    snprintf(buffer, buffer_size, "%02d:%02d:%02d", hours, minutes, seconds);
}

bool utils_check_nyquist_warning(double freq_to_check_hz, double sample_rate_hz, const char* context_str) {
    if (!context_str || sample_rate_hz <= 0) {
        return true; // Cannot perform check, so allow continuation.
    }

    double nyquist_freq = sample_rate_hz / 2.0;

    if (fabs(freq_to_check_hz) > nyquist_freq) {
        log_warn("The '%s' of %.15g Hz exceeds the Nyquist frequency of %.15g Hz for the current sample rate.",
                 context_str, freq_to_check_hz, nyquist_freq);
        log_warn("This may cause aliasing and corrupt the signal.");

        int response;
        do {
            fprintf(stderr, "Continue anyway? (y/n): ");
            response = getchar();
            if (response == EOF) {
                fprintf(stderr, "\nEOF detected. Cancelling.\n");
                return false;
            }
            utils_clear_stdin();
            response = tolower(response);
            if (response == 'n') {
                log_info("Operation cancelled by user.");
                return false;
            }
        } while (response != 'y');
    }
    return true;
}

bool utils_check_file_exists(const char* full_path) {
    FILE* fp = fopen(full_path, "r");
    if (fp) {
        fclose(fp);
        return true;
    }
    return false;
}

