/**
 * @file        system/checkpoint.h
 * @brief       Cooperative guest checkpoint boundary probe
 */

#pragma once

#include <cstdint>

struct PPCContext;

namespace rex::system {

struct CheckpointProbeResult {
  bool reached = false;
  uint32_t thread_id = 0;
  uint32_t guest_address = 0;
  uint32_t guest_lr = 0;
  uint32_t guest_stack_pointer = 0;
};

/** Called at the entry of each generated guest function. */
void PollCheckpointBoundary(PPCContext& context, uint32_t guest_address);

/** Park and resume the main guest thread at a generated function boundary. */
CheckpointProbeResult ProbeMainThreadCheckpoint(uint32_t timeout_ms = 2000);

}  // namespace rex::system
