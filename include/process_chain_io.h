/**
 * @file process_chain_stages.h
 * @brief Declares the high-speed data processing stages for the DSP
 * process_chain.
 */

#ifndef PROCESS_CHAIN_IO_H_
#define PROCESS_CHAIN_IO_H_

void *process_chain_thread_source(void *arg);
void *process_chain_thread_reader(void *arg);
void *process_chain_thread_writer(void *arg);

// --- DSP Pipeline Stage Interface ---
struct ThreadManager;
struct DspModuleInterface;
struct Queue;

bool process_chain_start_dsp_stage(struct ThreadManager *tm,
                                   const struct DspModuleInterface *module,
                                   void *state, struct Queue *in_q,
                                   struct Queue *out_q,
                                   struct Queue *free_chunk_q);

#endif // PROCESS_CHAIN_IO_H_
