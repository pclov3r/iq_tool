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

#endif // PROCESS_CHAIN_IO_H_
