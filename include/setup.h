// File: setup.h

#ifndef SETUP_H_
#define SETUP_H_

#include "types.h"

// --- Function Declarations for Setup Steps ---
bool initialize_application(AppConfig *config, AppResources *resources);
void cleanup_application(AppConfig *config, AppResources *resources);

bool resolve_file_paths(AppConfig *config);
bool calculate_and_validate_resample_ratio(AppConfig *config, AppResources *resources, float *out_ratio);
bool allocate_processing_buffers(AppConfig *config, AppResources *resources, float resample_ratio);
bool create_dsp_components(AppConfig *config, AppResources *resources, float resample_ratio);
bool create_threading_components(AppResources *resources);
void destroy_threading_components(AppResources *resources);
void print_configuration_summary(const AppConfig *config, const AppResources *resources);
bool check_nyquist_warning(const AppConfig *config, const AppResources *resources);
bool prepare_output_stream(AppConfig *config, AppResources *resources);

#endif // SETUP_H_
