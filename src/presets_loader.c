#include "presets_loader.h"
#include "constants.h"
#include "log.h"
#include "app_context.h"
#include "utils.h"
#include "platform.h"
#include "memory_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h> // For PathFileExistsW
#include <io.h>      // For _get_osfhandle, _fileno
#define strcasecmp _stricmp
#else
#include <strings.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <wordexp.h>
#include <libgen.h>
#endif

// --- The Dispatch Table (Unchanged) ---
static const PresetKeyHandler key_handlers[] = {
    { "description",      PRESET_KEY_STRDUP, offsetof(PresetDefinition, description),         0 },
    { "target_rate",      PRESET_KEY_STRTOD, offsetof(PresetDefinition, target_rate),         0 },
    { "sample_format_name", PRESET_KEY_STRDUP, offsetof(PresetDefinition, sample_format_name),  0 },
    { "output_type",      PRESET_KEY_OUTPUT_TYPE, offsetof(PresetDefinition, output_type),       0 },
    { "gain",             PRESET_KEY_STRTOF, offsetof(PresetDefinition, gain),              offsetof(PresetDefinition, gain_provided) },
    { "dc_block",         PRESET_KEY_BOOL,   offsetof(PresetDefinition, dc_block_enable),   offsetof(PresetDefinition, dc_block_provided) },
    { "iq_correction",    PRESET_KEY_BOOL,   offsetof(PresetDefinition, iq_correction_enable),offsetof(PresetDefinition, iq_correction_provided) },
    { "lowpass",          PRESET_KEY_STRTOF, offsetof(PresetDefinition, lowpass_cutoff_hz), offsetof(PresetDefinition, lowpass_cutoff_hz_provided) },
    { "highpass",         PRESET_KEY_STRTOF, offsetof(PresetDefinition, highpass_cutoff_hz),offsetof(PresetDefinition, highpass_cutoff_hz_provided) },
    { "pass_range",       PRESET_KEY_STRDUP, offsetof(PresetDefinition, pass_range_str),    offsetof(PresetDefinition, pass_range_str_provided) },
    { "stopband",         PRESET_KEY_STRDUP, offsetof(PresetDefinition, stopband_str),      offsetof(PresetDefinition, stopband_str_provided) },
    { "transition_width", PRESET_KEY_STRTOF, offsetof(PresetDefinition, transition_width_hz), offsetof(PresetDefinition, transition_width_hz_provided) },
    { "filter_taps",      PRESET_KEY_STRTOL, offsetof(PresetDefinition, filter_taps),       offsetof(PresetDefinition, filter_taps_provided) },
    { "attenuation",      PRESET_KEY_STRTOF, offsetof(PresetDefinition, attenuation_db),    offsetof(PresetDefinition, attenuation_db_provided) },
    { "filter_type",      PRESET_KEY_STRDUP, offsetof(PresetDefinition, filter_type_str),   offsetof(PresetDefinition, filter_type_str_provided) },
};
static const size_t num_key_handlers = sizeof(key_handlers) / sizeof(key_handlers[0]);


// --- Helper function to duplicate a string using the memory arena ---
static char* arena_strdup(MemoryArena* arena, const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* new_s = (char*)mem_arena_alloc(arena, len, false);
    if (new_s) {
        memcpy(new_s, s, len);
    }
    return new_s;
}

bool presets_load_from_file(AppConfig* config, MemoryArena* arena) {
    config->presets = NULL;
    config->num_presets = 0;

    char full_path_buffer[MAX_PATH_BUFFER];
    
    char* found_preset_files[5];
    int num_found_files = 0;

    const char* search_paths_list[10];
    int current_path_idx = 0;

#ifdef _WIN32
    char exe_dir[MAX_PATH_BUFFER];
    if (platform_get_executable_dir(exe_dir, sizeof(exe_dir))) {
        search_paths_list[current_path_idx++] = exe_dir;
    }
    wchar_t* appdata_path_w = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata_path_w) == S_OK) {
        wchar_t full_appdata_path_w[MAX_PATH_BUFFER];
        wcsncpy(full_appdata_path_w, appdata_path_w, MAX_PATH_BUFFER - 1);
        full_appdata_path_w[MAX_PATH_BUFFER - 1] = L'\0';
        CoTaskMemFree(appdata_path_w);
        PathAppendW(full_appdata_path_w, L"\\" APP_NAME);
        
        char* appdata_path_utf8 = (char*)mem_arena_alloc(arena, MAX_PATH_BUFFER, false);
        if (appdata_path_utf8) {
            if (WideCharToMultiByte(CP_UTF8, 0, full_appdata_path_w, -1, appdata_path_utf8, MAX_PATH_BUFFER, NULL, NULL) > 0) {
                search_paths_list[current_path_idx++] = appdata_path_utf8;
            } else {
                log_warn("Failed to convert AppData path to UTF-8 for presets.");
            }
        } else {
            return false;
        }
    }
    wchar_t* programdata_path_w = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_ProgramData, 0, NULL, &programdata_path_w) == S_OK) {
        wchar_t full_programdata_path_w[MAX_PATH_BUFFER];
        wcsncpy(full_programdata_path_w, programdata_path_w, MAX_PATH_BUFFER - 1);
        full_programdata_path_w[MAX_PATH_BUFFER - 1] = L'\0';
        CoTaskMemFree(programdata_path_w);
        PathAppendW(full_programdata_path_w, L"\\" APP_NAME);

        char* programdata_path_utf8 = (char*)mem_arena_alloc(arena, MAX_PATH_BUFFER, false);
        if (programdata_path_utf8) {
            if (WideCharToMultiByte(CP_UTF8, 0, full_programdata_path_w, -1, programdata_path_utf8, MAX_PATH_BUFFER, NULL, NULL) > 0) {
                search_paths_list[current_path_idx++] = programdata_path_utf8;
            } else {
                log_warn("Failed to convert ProgramData path to UTF-8 for presets.");
            }
        } else {
            return false;
        }
    }
#else // POSIX
    search_paths_list[current_path_idx++] = ".";
    const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
    
    char* xdg_path = (char*)mem_arena_alloc(arena, MAX_PATH_BUFFER, false);
    if (!xdg_path) {
        return false;
    }
    
    bool xdg_path_set = false;
    if (xdg_config_home && xdg_config_home[0] != '\0') {
        snprintf(xdg_path, MAX_PATH_BUFFER, "%s/%s", xdg_config_home, APP_NAME);
        xdg_path_set = true;
    } else {
        const char* home_dir = getenv("HOME");
        if (home_dir) {
            snprintf(xdg_path, MAX_PATH_BUFFER, "%s/.config/%s", home_dir, APP_NAME);
            xdg_path_set = true;
        }
    }
    if (xdg_path_set) {
        search_paths_list[current_path_idx++] = xdg_path;
    }
    
    search_paths_list[current_path_idx++] = "/etc/" APP_NAME;
    search_paths_list[current_path_idx++] = "/usr/local/etc/" APP_NAME;
#endif

    for (int i = 0; i < current_path_idx; ++i) {
        const char* base_dir = search_paths_list[i];
        if (base_dir == NULL) continue;
        snprintf(full_path_buffer, sizeof(full_path_buffer), "%s/%s", base_dir, PRESETS_FILENAME);
        
        bool file_is_safe_and_exists = false;
        #ifdef _WIN32
        wchar_t full_path_w[MAX_PATH_BUFFER];
        if (MultiByteToWideChar(CP_UTF8, 0, full_path_buffer, -1, full_path_w, MAX_PATH_BUFFER) > 0) {
            DWORD attrs = GetFileAttributesW(full_path_w);
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                if (!(attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    file_is_safe_and_exists = true;
                } else {
                    log_warn("Security: Presets file candidate '%s' is not a regular file (e.g., is a directory or symlink). Skipping.", full_path_buffer);
                }
            }
        }
        #else
        struct stat file_stat;
        if (lstat(full_path_buffer, &file_stat) == 0) {
            if (S_ISREG(file_stat.st_mode)) {
                file_is_safe_and_exists = true;
            } else {
                log_warn("Security: Presets file candidate '%s' is not a regular file (e.g., is a directory or symlink). Skipping.", full_path_buffer);
            }
        }
        #endif

        if (file_is_safe_and_exists) {
            if (num_found_files < (int)(sizeof(found_preset_files) / sizeof(found_preset_files[0]))) {
                found_preset_files[num_found_files] = arena_strdup(arena, full_path_buffer);
                if (!found_preset_files[num_found_files]) {
                    return false;
                }
                num_found_files++;
            }
        }
    }

    if (num_found_files > 1) {
        log_warn("Conflicting presets files found. No presets will be loaded. Please resolve the conflict by keeping only one of the following files:");
        for (int i = 0; i < num_found_files; ++i) {
            log_warn("  - %s", found_preset_files[i]);
        }
        return true;
    } else if (num_found_files == 0) {
        log_info("No presets file '%s' found in any standard location. No external presets will be available.", PRESETS_FILENAME);
        return true;
    }

    FILE* fp = NULL;
    #ifdef _WIN32
    wchar_t preset_file_w[MAX_PATH_BUFFER];
    if (MultiByteToWideChar(CP_UTF8, 0, found_preset_files[0], -1, preset_file_w, MAX_PATH_BUFFER) > 0) {
        fp = _wfopen(preset_file_w, L"r");
    }
    #else
    fp = fopen(found_preset_files[0], "r");
    #endif

    if (!fp) {
        log_fatal("Error opening presets file '%s': %s", found_preset_files[0], strerror(errno));
        return false;
    }

    #ifdef _WIN32
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(fp));
    BY_HANDLE_FILE_INFORMATION fileInfo;
    if (GetFileInformationByHandle(hFile, &fileInfo)) {
        if (fileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            log_fatal("Security: Presets file '%s' is a directory. Aborting.", found_preset_files[0]);
            fclose(fp);
            return false;
        }
    } else {
        log_fatal("Could not get file status for '%s'", found_preset_files[0]);
        fclose(fp);
        return false;
    }
    #else
    struct stat file_stat;
    if (fstat(fileno(fp), &file_stat) != 0) {
        log_fatal("Could not get file status for '%s': %s", found_preset_files[0], strerror(errno));
        fclose(fp);
        return false;
    }
    if (!S_ISREG(file_stat.st_mode)) {
        log_fatal("Security: Presets file '%s' is not a regular file. Aborting.", found_preset_files[0]);
        fclose(fp);
        return false;
    }
    #endif

    char line[MAX_LINE_LENGTH];
    PresetDefinition* current_preset = NULL;
    int capacity = 8;

    config->presets = (PresetDefinition*)mem_arena_alloc(arena, capacity * sizeof(PresetDefinition), true);
    if (!config->presets) {
        fclose(fp);
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
                int old_capacity = capacity;
                capacity *= 2;
                PresetDefinition* new_presets = (PresetDefinition*)mem_arena_alloc(arena, capacity * sizeof(PresetDefinition), true);
                if (!new_presets) {
                    fclose(fp);
                    return false;
                }
                memcpy(new_presets, config->presets, old_capacity * sizeof(PresetDefinition));
                config->presets = new_presets;
            }
            current_preset = &config->presets[config->num_presets];
            memset(current_preset, 0, sizeof(PresetDefinition));
            char* name_start = trimmed_line + strlen("[preset:");
            char* name_end = strchr(name_start, ']');
            if (name_end) {
                *name_end = '\0';
                current_preset->name = arena_strdup(arena, trim_whitespace(name_start));
                if (!current_preset->name) {
                    fclose(fp);
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

            bool key_found = false;
            for (size_t i = 0; i < num_key_handlers; ++i) {
                const PresetKeyHandler* handler = &key_handlers[i];
                if (strcasecmp(key, handler->key_name) == 0) {
                    char* value_ptr = (char*)current_preset + handler->value_offset;
                    bool* provided_ptr = (bool*)((char*)current_preset + handler->provided_flag_offset);

                    switch (handler->action) {
                        case PRESET_KEY_STRDUP:
                            *(char**)value_ptr = arena_strdup(arena, value);
                            if (!*(char**)value_ptr) {
                                fclose(fp);
                                return false;
                            }
                            break;
                        case PRESET_KEY_STRTOD:
                            *(double*)value_ptr = strtod(value, NULL);
                            break;
                        case PRESET_KEY_STRTOF:
                            *(float*)value_ptr = strtof(value, NULL);
                            break;
                        case PRESET_KEY_STRTOL:
                            *(int*)value_ptr = (int)strtol(value, NULL, 10);
                            break;
                        case PRESET_KEY_BOOL:
                            if (strcasecmp(value, "true") == 0) *(bool*)value_ptr = true;
                            else if (strcasecmp(value, "false") == 0) *(bool*)value_ptr = false;
                            break;
                        case PRESET_KEY_OUTPUT_TYPE:
                            if (strcasecmp(value, "raw") == 0) *(OutputType*)value_ptr = OUTPUT_TYPE_RAW;
                            else if (strcasecmp(value, "wav") == 0) *(OutputType*)value_ptr = OUTPUT_TYPE_WAV;
                            else if (strcasecmp(value, "wav-rf64") == 0) *(OutputType*)value_ptr = OUTPUT_TYPE_WAV_RF64;
                            break;
                    }

                    if (handler->provided_flag_offset > 0) {
                        *provided_ptr = true;
                    }
                    key_found = true;
                    break;
                }
            }

            if (!key_found) {
                log_warn("Unknown key '%s' in preset '%s' at line %d.", key, current_preset->name, line_num);
            }
        }
    }

    fclose(fp);
    return true;
}
