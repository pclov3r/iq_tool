/*
Copyright (c) 2016-2023, Youssef Touil <youssef@airspy.com>
Copyright (c) 2018, Leif Asbrink <leif@sm5bsz.com>

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

		Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
		Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the
		documentation and/or other materials provided with the distribution.
		Neither the name of Airspy HF+ nor the names of its contributors may be used to endorse or promote products derived from this software
		without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/**
 * @file iq_correct.h
 * @brief Defines the interface for the automatic I/Q imbalance correction module.
 *
 * This module adapts the Airspy HF+ / SM5BSZ correlation-based estimator
 * for use within the iq_tool pipeline. It detects and corrects gain and phase
 * imbalances by analyzing the signal spectrum.
 */

#ifndef IQ_CORRECT_H_
#define IQ_CORRECT_H_

#include <stdbool.h>
#include "app_context.h"
#include "memory_arena.h"
#include <sndfile.h> // Needed for the SNDFILE* type in the function signature

// --- Function Declarations ---

/**
 * @brief Initializes the I/Q correction module.
 *
 * Allocates internal state buffers and FFT plans required by the SM5BSZ algorithm.
 *
 * @param config Pointer to the application configuration.
 * @param resources Pointer to the application resources where correction state will be stored.
 * @param arena The memory arena to use for all buffer allocations.
 * @return true on success or if disabled, false on failure.
 */
bool iq_correct_init(AppConfig* config, AppResources* resources, MemoryArena* arena);

/**
 * @brief Applies the current I/Q imbalance correction to a block of samples.
 *
 * This function performs the "Fast Path" processing:
 * 1. Interpolates phase/amplitude correction factors.
 * 2. Applies the correction matrix to the samples in-place.
 *
 * @param resources Pointer to the application resources.
 * @param samples Pointer to the complex float samples (modified in-place).
 * @param num_samples The number of complex samples in the block.
 */
void iq_correct_apply(AppResources* resources, complex_float_t* samples, int num_samples);

/**
 * @brief Runs the I/Q imbalance optimization/estimation algorithm.
 *
 * This function performs the "Slow Path" processing:
 * 1. Performs an FFT on the input data.
 * 2. Calculates correlation between signal and image.
 * 3. Updates the global phase/amplitude correction targets.
 *
 * @param resources Pointer to the application resources.
 * @param optimization_data Pointer to the block of complex float samples to analyze.
 */
void iq_correct_run_optimization(AppResources* resources, const complex_float_t* optimization_data);

/**
 * @brief Cleans up resources allocated by the I/Q correction module.
 *
 * @param resources Pointer to the application resources.
 */
void iq_correct_destroy(AppResources* resources);

/**
 * @brief Performs a synchronous, one-shot I/Q calibration pass for file-based inputs.
 *
 * @param ctx The application context.
 * @param infile The handle to the open input file (e.g., from libsndfile).
 * @return true on success, false on a critical failure.
 */
bool iq_correct_run_initial_calibration(ModuleContext* ctx, SNDFILE* infile);

#endif // IQ_CORRECT_H_
