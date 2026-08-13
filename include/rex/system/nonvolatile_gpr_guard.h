/**
 * @file        system/nonvolatile_gpr_guard.h
 * @brief       Guest-call ABI diagnostics for nonvolatile GPRs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#pragma once

#include <array>
#include <cstdint>

#include <rex/ppc/context.h>

namespace rex::runtime {

struct NonvolatileGprDifference {
  uint32_t register_index = 0;
  uint64_t before = 0;
  uint64_t after = 0;

  explicit operator bool() const noexcept { return register_index != 0; }
};

/** Copy the process-wide GPR-guard configuration into a new guest context. */
void ConfigureNonvolatileGprGuardContext(PPCContext& context) noexcept;

/** A snapshot of the ABI-preserved guest GPRs r14 through r31. */
struct NonvolatileGprSnapshot {
  static constexpr uint32_t kFirstRegister = 14;
  static constexpr uint32_t kRegisterCount = 18;

  std::array<uint64_t, kRegisterCount> values{};

  void Capture(const PPCContext& context) noexcept;
  NonvolatileGprDifference FindFirstDifference(const PPCContext& context) const noexcept;
};

/**
 * Check one normal guest-call return against the PPC ABI.
 *
 * The generator does not use this guard for compiler save and restore helpers.
 */
class NonvolatileGprCallGuard {
 public:
  NonvolatileGprCallGuard(const PPCContext& context, uint32_t caller, uint32_t callsite,
                          uint32_t target) noexcept;

  void Check(const PPCContext& context) const noexcept;

 private:
  bool enabled_ = false;
  uint32_t caller_ = 0;
  uint32_t callsite_ = 0;
  uint32_t target_ = 0;
  NonvolatileGprSnapshot snapshot_{};
};

}  // namespace rex::runtime
