/**
 * @file pipeline_stages.h
 * @brief Declares the high-speed data processing stages for the DSP pipeline.
 */

#ifndef PIPELINE_STAGES_H_
#define PIPELINE_STAGES_H_

void* pipeline_thread_source(void* arg);
void* pipeline_thread_reader(void* arg);
void* pipeline_thread_pre_processor(void* arg);
void* pipeline_thread_resampler(void* arg);
void* pipeline_thread_post_processor(void* arg);
void* pipeline_thread_writer(void* arg);

#endif // PIPELINE_STAGES_H_
