// presets_loader.c
#include "presets_loader.h"
#include "log.h"
#include "config.h"
#include "utils.h"
#include "platform.h" // <<< ADDED
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#define strcasecmp _stricmp
#else
#include <strings.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <wordexp.h>
#include <libgen.h>
#endif

// --- Helper function declarations ---
static void free_dynamic_paths(char** paths, int count);


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
 */
bool presets_load_from_file(AppConfig* config) {
    // Initialize config fields
    config->presets = NULL;
    config->num_presets = 0;

    char full_path_buffer[MAX_PATH_BUFFER];
    char* dynamic_paths[5] = {NULL};
    int dynamic_paths_count = 0;

    char* found_preset_files[5];
    int num_found_files = 0;

    const char* search_paths_list[10];
    int current_path_idx = 0;

#ifdef _WIN32
    // 1. Executable directory
    char exe_dir[MAX_PATH_BUFFER];
    if (platform_get_executable_dir(exe_dir, sizeof(exe_dir))) { // <<< CHANGED
        search_paths_list[current_path_idx++] = exe_dir;
    }

    // 2. %APPDATA%\APP_NAME
    wchar_t* appdata_path_w = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata_path_w) == S_OK) {
        wchar_t full_appdata_path_w[MAX_PATH_BUFFER];
        wcsncpy(full_appdata_path_w, appdata_path_w, MAX_PATH_BUFFER - 1);
        full_appdata_path_w[MAX_PATH_BUFFER - 1] = L'\0';
        CoTaskMemFree(appdata_path_w);

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
    // ... (POSIX paths remain the same) ...
    search_paths_list[current_path_idx++] = ".";
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
        const char* home_dir = getenv("HOME");
        if (home_dir) {
            snprintf(xdg_path, MAX_PATH_BUFFER, "%s/.config/%s", home_dir, APP_NAME);
        } else {
            free(xdg_path);
            xdg_path = NULL;
        }
    }
    if (xdg_path) {
        dynamic_paths[dynamic_paths_count++] = xdg_path;
        search_paths_list[current_path_idx++] = xdg_path;
    }
    search_paths_list[current_path_idx++] = "/etc/" APP_NAME;
    search_paths_list[current_path_idx++] = "/usr/local/etc/" APP_NAME;
#endif

    // --- Search for the presets file in all defined locations ---
    for (int i = 0; i < current_path_idx; ++i) {
        const char* base_dir = search_paths_list[i];
        if (base_dir == NULL) continue;

        snprintf(full_path_buffer, sizeof(full_path_buffer), "%s/%s", base_dir, PRESETS_FILENAME);

        if (utils_check_file_exists(full_path_buffer)) { // <<< CHANGED
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

    // ... (The rest of the function remains unchanged) ...
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
            } else if (strcasecmp(key, "gain") == 0) {
                char* endptr;
                float parsed_gain = strtof(value, &endptr);
                if (*endptr == '\0' && parsed_gain > 0.0f && isfinite(parsed_gain)) {
                    current_preset->gain = parsed_gain;
                    current_preset->gain_provided = true;
                } else {
                    log_warn("Invalid value for 'gain' in preset '%s' at line %d: '%s'", current_preset->name, line_num, value);
                }
            } else if (strcasecmp(key, "dc_block") == 0) {
                if (strcasecmp(value, "true") == 0) {
                    current_preset->dc_block_enable = true;
                    current_preset->dc_block_provided = true;
                } else if (strcasecmp(value, "false") == 0) {
                    current_preset->dc_block_enable = false;
                    current_preset->dc_block_provided = true;
                } else {
                    log_warn("Invalid value for 'dc_block' in preset '%s' at line %d: '%s'. Use 'true' or 'false'.", current_preset->name, line_num, value);
                }
            } else if (strcasecmp(key, "iq_correction") == 0) {
                if (strcasecmp(value, "true") == 0) {
                    current_preset->iq_correction_enable = true;
                    current_preset->iq_correction_provided = true;
                } else if (strcasecmp(value, "false") == 0) {
                    current_preset->iq_correction_enable = false;
                    current_preset->iq_correction_provided = true;
                } else {
                    log_warn("Invalid value for 'iq_correction' in preset '%s' at line %d: '%s'. Use 'true' or 'false'.", current_preset->name, line_num, value);
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
