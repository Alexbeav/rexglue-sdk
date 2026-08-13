/**
 * @file        system/nonvolatile_gpr_guard.cpp
 * @brief       Guest-call ABI diagnostics for nonvolatile GPRs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/nonvolatile_gpr_guard.h>

REXCVAR_DEFINE_BOOL(nonvolatile_gpr_guard, false, "Debug",
                    "Stop when a normal guest call changes r14 through r31")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::runtime {

void ConfigureNonvolatileGprGuardContext(PPCContext& context) noexcept {
  context.nonvolatile_gpr_guard = REXCVAR_GET(nonvolatile_gpr_guard);
}

void NonvolatileGprSnapshot::Capture(const PPCContext& context) noexcept {
  const auto* first = &context.r14;
  for (uint32_t index = 0; index < kRegisterCount; ++index) {
    values[index] = first[index].u64;
  }
}

NonvolatileGprDifference NonvolatileGprSnapshot::FindFirstDifference(
    const PPCContext& context) const noexcept {
  const auto* first = &context.r14;
  for (uint32_t index = 0; index < kRegisterCount; ++index) {
    if (values[index] != first[index].u64) {
      return {
          .register_index = kFirstRegister + index,
          .before = values[index],
          .after = first[index].u64,
      };
    }
  }
  return {};
}

NonvolatileGprCallGuard::NonvolatileGprCallGuard(const PPCContext& context, uint32_t caller,
                                                 uint32_t callsite, uint32_t target) noexcept
    : enabled_(context.nonvolatile_gpr_guard),
      caller_(caller),
      callsite_(callsite),
      target_(target) {
  if (enabled_) {
    snapshot_.Capture(context);
  }
}

void NonvolatileGprCallGuard::Check(const PPCContext& context) const noexcept {
  if (!enabled_) {
    return;
  }

  const auto difference = snapshot_.FindFirstDifference(context);
  if (!difference) {
    return;
  }

  REX_FATAL(
      "[AVDIAG] nonvolatile GPR mismatch: caller=0x{:08X} callsite=0x{:08X} "
      "target=0x{:08X} register=r{} before=0x{:016X} after=0x{:016X}",
      caller_, callsite_, target_, difference.register_index, difference.before, difference.after);
}

}  // namespace rex::runtime
