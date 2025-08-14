#include "presets_loader.h"
#include "log.h"
#include "config.h" // Include config.h for APP_NAME, PRESETS_FILENAME, and MAX_PATH_BUFFER
#include "utils.h"  // For trim_whitespace
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h> // For strerror
#include <math.h> // For isfinite

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h> // For SHGetKnownFolderPath
#include <shlwapi.h> // For PathAppendW
#pragma comment(lib, "shell32.lib") // Link with shell32.lib for SHGetKnownFolderPath
#pragma comment(lib, "shlwapi.lib") // Link with shlwapi.lib for PathAppendW
#define strcasecmp _stricmp
#else
#include <strings.h> // For strcasecmp
#include <unistd.h>  // For readlink
#include <limits.h>  // For PATH_MAX (used by wordexp, but MAX_PATH_BUFFER is preferred for buffers)
#include <sys/stat.h> // For stat
#include <wordexp.h> // For wordexp (to expand ~)
#include <libgen.h> // For dirname
#endif

// --- Security and Robustness Constants ---
// REMOVED: These are now in config.h
// #define MAX_LINE_LENGTH 1024 // A reasonable limit for a single line in the config file
// #define MAX_PRESETS 128      // A sanity limit on the total number of presets to load


// --- Helper function declarations ---
static bool check_file_exists(const char* full_path);
static void free_dynamic_paths(char** paths, int count);

#ifdef _WIN32
/**
 * @brief Helper function to get the executable's directory (Windows specific).
 * @param buffer Buffer to store the path.
 * @param buffer_size Size of the buffer.
 * @return true on success, false on failure.
 */
static bool get_executable_dir(char* buffer, size_t buffer_size) {
    wchar_t w_path[MAX_PATH_BUFFER]; // Use MAX_PATH_BUFFER for consistency
    DWORD len = GetModuleFileNameW(NULL, w_path, MAX_PATH_BUFFER);
    if (len == 0 || len == MAX_PATH_BUFFER) {
        log_error("GetModuleFileNameW failed or buffer too small.");
        return false;
    }
    // Remove filename to get directory
    wchar_t* last_slash = wcsrchr(w_path, L'\\');
    if (last_slash) {
        *last_slash = L'\0';
    } else {
        // No slash, probably just the executable name, so current dir
        wcsncpy(w_path, L".", MAX_PATH_BUFFER);
        w_path[MAX_PATH_BUFFER - 1] = L'\0';
    }
    if (WideCharToMultiByte(CP_UTF8, 0, w_path, -1, buffer, buffer_size, NULL, NULL) == 0) {
        log_error("Failed to convert wide char path to UTF-8 for executable directory.");
        return false;
    }
    return true;
}
#endif // _WIN32

/**
 * @brief Helper function to check if a file exists at the given path.
 * @param full_path The full path to the file.
 * @return true if the file exists and is readable, false otherwise.
 */
static bool check_file_exists(const char* full_path) {
    FILE* fp = fopen(full_path, "r");
    if (fp) {
        fclose(fp);
        return true;
    }
    return false;
}

/**
 * @brief Helper function to free dynamically allocated path strings.
 */
static void free_dynamic_paths(char** paths, int count) {
    for (int i = 0; i < count; ++i) {
        free(paths[i]);
        paths[i] = NULL;
    }
}

/**
 * @brief Loads preset definitions from a text file, searching common locations.
 * @param config A pointer to the AppConfig struct where the loaded presets will be stored.
 * @return true on success (even if no file is found or a conflict is warned), false on a fatal error.
 */
bool presets_load_from_file(AppConfig* config) {
    // Initialize config fields
    config->presets = NULL;
    config->num_presets = 0;

    char full_path_buffer[MAX_PATH_BUFFER];
    // To store dynamically allocated paths (e.g., from SHGetKnownFolderPath, getenv, wordexp)
    char* dynamic_paths[5] = {NULL}; // Max 5 dynamic paths (e.g., 3 for Windows, 3 for POSIX)
    int dynamic_paths_count = 0;

    // List to store paths where the presets file was found
    char* found_preset_files[5]; // Max 5 found files (e.g., 1 per search path type)
    int num_found_files = 0;

    // Define potential search paths.
    // Note: search_paths_list will contain pointers to either string literals or dynamic_paths.
    const char* search_paths_list[10]; // Max 10 potential paths
    int current_path_idx = 0;

#ifdef _WIN32
    // 1. Executable directory
    char exe_dir[MAX_PATH_BUFFER];
    if (get_executable_dir(exe_dir, sizeof(exe_dir))) {
        search_paths_list[current_path_idx++] = exe_dir;
    }

    // 2. %APPDATA%\APP_NAME
    wchar_t* appdata_path_w = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata_path_w) == S_OK) {
        wchar_t full_appdata_path_w[MAX_PATH_BUFFER];
        wcsncpy(full_appdata_path_w, appdata_path_w, MAX_PATH_BUFFER - 1);
        full_appdata_path_w[MAX_PATH_BUFFER - 1] = L'\0';
        CoTaskMemFree(appdata_path_w); // Free the path returned by the function

        PathAppendW(full_appdata_path_w, L"\\" APP_NAME);
        char* appdata_path_utf8 = (char*)malloc(MAX_PATH_BUFFER);
        if (appdata_path_utf8) {
            if (WideCharToMultiByte(CP_UTF8, 0, full_appdata_path_w, -1, appdata_path_utf8, MAX_PATH_BUFFER, NULL, NULL) > 0) {
                dynamic_paths[dynamic_paths_count++] = appdata_path_utf8;
                search_paths_list[current_path_idx++] = appdata_path_utf8;
            } else {
                free(appdata_path_utf8);
                log_warn("Failed to convert AppData path to UTF-8 for presets.");
            }
        } else {
            log_fatal("Failed to allocate memory for AppData path.");
            free_dynamic_paths(dynamic_paths, dynamic_paths_count);
            return false;
        }
    }

    // 3. %PROGRAMDATA%\APP_NAME
    wchar_t* programdata_path_w = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_ProgramData, 0, NULL, &programdata_path_w) == S_OK) {
        wchar_t full_programdata_path_w[MAX_PATH_BUFFER];
        wcsncpy(full_programdata_path_w, programdata_path_w, MAX_PATH_BUFFER - 1);
        full_programdata_path_w[MAX_PATH_BUFFER - 1] = L'\0';
        CoTaskMemFree(programdata_path_w);

        PathAppendW(full_programdata_path_w, L"\\" APP_NAME);
        char* programdata_path_utf8 = (char*)malloc(MAX_PATH_BUFFER);
        if (programdata_path_utf8) {
            if (WideCharToMultiByte(CP_UTF8, 0, full_programdata_path_w, -1, programdata_path_utf8, MAX_PATH_BUFFER, NULL, NULL) > 0) {
                dynamic_paths[dynamic_paths_count++] = programdata_path_utf8;
                search_paths_list[current_path_idx++] = programdata_path_utf8;
            } else {
                free(programdata_path_utf8);
                log_warn("Failed to convert ProgramData path to UTF-8 for presets.");
            }
        } else {
            log_fatal("Failed to allocate memory for ProgramData path.");
            free_dynamic_paths(dynamic_paths, dynamic_paths_count);
            return false;
        }
    }
#else // POSIX
    // 1. Current working directory
    search_paths_list[current_path_idx++] = ".";

    // 2. XDG Base Directory Specification (user-specific)
    const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
    char* xdg_path = (char*)malloc(MAX_PATH_BUFFER);
    if (!xdg_path) {
        log_fatal("Failed to allocate memory for XDG path.");
        free_dynamic_paths(dynamic_paths, dynamic_paths_count);
        return false;
    }
    if (xdg_config_home && xdg_config_home[0] != '\0') {
        snprintf(xdg_path, MAX_PATH_BUFFER, "%s/%s", xdg_config_home, APP_NAME);
    } else {
        // Fallback to ~/.config/APP_NAME if XDG_CONFIG_HOME not set or is empty
        const char* home_dir = getenv("HOME");
        if (home_dir) {
            snprintf(xdg_path, MAX_PATH_BUFFER, "%s/.config/%s", home_dir, APP_NAME);
        } else {
            // No HOME, can't form path, so free and skip
            free(xdg_path);
            xdg_path = NULL;
        }
    }
    if (xdg_path) {
        dynamic_paths[dynamic_paths_count++] = xdg_path;
        search_paths_list[current_path_idx++] = xdg_path;
    }

    // 3. System-wide config locations
    search_paths_list[current_path_idx++] = "/etc/" APP_NAME;
    search_paths_list[current_path_idx++] = "/usr/local/etc/" APP_NAME;
#endif

    // --- Search for the presets file in all defined locations ---
    for (int i = 0; i < current_path_idx; ++i) {
        const char* base_dir = search_paths_list[i];
        if (base_dir == NULL) continue;

        // Construct full path
        snprintf(full_path_buffer, sizeof(full_path_buffer), "%s/%s", base_dir, PRESETS_FILENAME);

        if (check_file_exists(full_path_buffer)) {
            if (num_found_files < (int)(sizeof(found_preset_files) / sizeof(found_preset_files[0]))) {
                found_preset_files[num_found_files] = strdup(full_path_buffer);
                if (!found_preset_files[num_found_files]) {
                    log_fatal("Failed to duplicate found preset file path string.");
                    free_dynamic_paths(dynamic_paths, dynamic_paths_count);
                    for(int j=0; j<num_found_files; ++j) free(found_preset_files[j]);
                    return false;
                }
                num_found_files++;
            }
        }
    }

    // --- Handle conflict resolution or absence ---
    if (num_found_files > 1) {
        log_warn("Conflicting presets files found. No presets will be loaded. Please resolve the conflict by keeping only one of the following files:");
        for (int i = 0; i < num_found_files; ++i) {
            log_warn("  - %s", found_preset_files[i]);
            free(found_preset_files[i]);
        }
        free_dynamic_paths(dynamic_paths, dynamic_paths_count);
        return true;
    } else if (num_found_files == 0) {
        log_info("No presets file '%s' found in any standard location. No external presets will be available.", PRESETS_FILENAME);
        free_dynamic_paths(dynamic_paths, dynamic_paths_count);
        return true;
    }

    // --- Load the single found presets file ---
    FILE* fp = fopen(found_preset_files[0], "r");
    if (!fp) {
        log_fatal("Error opening presets file '%s': %s", found_preset_files[0], strerror(errno));
        free(found_preset_files[0]);
        free_dynamic_paths(dynamic_paths, dynamic_paths_count);
        return false;
    }

    char line[MAX_LINE_LENGTH];
    PresetDefinition* current_preset = NULL;
    int capacity = 8;

    config->presets = malloc(capacity * sizeof(PresetDefinition));
    if (!config->presets) {
        log_fatal("Failed to allocate initial memory for presets.");
        fclose(fp);
        free(found_preset_files[0]);
        free_dynamic_paths(dynamic_paths, dynamic_paths_count);
        return false;
    }

    int line_num = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        char* trimmed_line = trim_whitespace(line);

        if (trimmed_line[0] == '#' || trimmed_line[0] == ';' || trimmed_line[0] == '\0') {
            continue;
        }

        if (trimmed_line[0] == '[' && strstr(trimmed_line, "preset:")) {
            if (config->num_presets >= MAX_PRESETS) {
                log_warn("Maximum number of presets (%d) reached at line %d. Ignoring further presets.", MAX_PRESETS, line_num);
                current_preset = NULL;
                continue;
            }

            if (config->num_presets == capacity) {
                capacity *= 2;
                PresetDefinition* new_presets = realloc(config->presets, capacity * sizeof(PresetDefinition));
                if (!new_presets) {
                    log_fatal("Failed to reallocate memory for presets.");
                    presets_free_loaded(config);
                    fclose(fp);
                    free(found_preset_files[0]);
                    free_dynamic_paths(dynamic_paths, dynamic_paths_count);
                    return false;
                }
                config->presets = new_presets;
            }

            current_preset = &config->presets[config->num_presets];
            memset(current_preset, 0, sizeof(PresetDefinition));

            char* name_start = trimmed_line + strlen("[preset:");
            char* name_end = strchr(name_start, ']');
            if (name_end) {
                *name_end = '\0';
                current_preset->name = strdup(trim_whitespace(name_start));
                if (!current_preset->name) {
                    log_fatal("Failed to duplicate preset name string.");
                    presets_free_loaded(config);
                    fclose(fp);
                    free(found_preset_files[0]);
                    free_dynamic_paths(dynamic_paths, dynamic_paths_count);
                    return false;
                }
                config->num_presets++;
            } else {
                log_warn("Malformed preset header at line %d: %s", line_num, trimmed_line);
                current_preset = NULL;
            }
        } else if (current_preset && strchr(trimmed_line, '=')) {
            char* key = strtok(trimmed_line, "=");
            char* value = strtok(NULL, "");

            if (!key || !value) {
                log_warn("Malformed key-value pair at line %d.", line_num);
                continue;
            }
            key = trim_whitespace(key);
            value = trim_whitespace(value);

            if (strcasecmp(key, "description") == 0) {
                current_preset->description = strdup(value);
                if (!current_preset->description) {
                    log_fatal("Failed to duplicate preset description string.");
                    presets_free_loaded(config);
                    fclose(fp);
                    free(found_preset_files[0]);
                    free_dynamic_paths(dynamic_paths, dynamic_paths_count);
                    return false;
                }
            } else if (strcasecmp(key, "target_rate") == 0) {
                char* endptr;
                current_preset->target_rate = strtod(value, &endptr);
                if (*endptr != '\0' || current_preset->target_rate <= 0 || !isfinite(current_preset->target_rate)) {
                    log_warn("Invalid value for 'target_rate' in preset '%s' at line %d: '%s'", current_preset->name, line_num, value);
                    current_preset->target_rate = 0.0;
                }
            } else if (strcasecmp(key, "sample_format_name") == 0) {
                current_preset->sample_format_name = strdup(value);
                if (!current_preset->sample_format_name) {
                    log_fatal("Failed to duplicate preset sample format name string.");
                    presets_free_loaded(config);
                    fclose(fp);
                    free(found_preset_files[0]);
                    free_dynamic_paths(dynamic_paths, dynamic_paths_count);
                    return false;
                }
            } else if (strcasecmp(key, "output_type") == 0) {
                if (strcasecmp(value, "raw") == 0) current_preset->output_type = OUTPUT_TYPE_RAW;
                else if (strcasecmp(value, "wav") == 0) current_preset->output_type = OUTPUT_TYPE_WAV;
                else if (strcasecmp(value, "wav-rf64") == 0) current_preset->output_type = OUTPUT_TYPE_WAV_RF64;
                else {
                    log_warn("Invalid value for 'output_type' in preset '%s' at line %d: '%s'", current_preset->name, line_num, value);
                }
            } else {
                log_warn("Unknown key '%s' in preset '%s' at line %d.", key, current_preset->name, line_num);
            }
        }
    }

    fclose(fp);
    free(found_preset_files[0]);
    free_dynamic_paths(dynamic_paths, dynamic_paths_count);
    return true;
}

/**
 * @brief Frees the memory allocated for the presets.
 */
void presets_free_loaded(AppConfig* config) {
    if (!config || !config->presets) return;

    for (int i = 0; i < config->num_presets; i++) {
        free(config->presets[i].name);
        config->presets[i].name = NULL;
        free(config->presets[i].description);
        config->presets[i].description = NULL;
        free(config->presets[i].sample_format_name);
        config->presets[i].sample_format_name = NULL;
    }
    free(config->presets);
    config->presets = NULL;
    config->num_presets = 0;
}
