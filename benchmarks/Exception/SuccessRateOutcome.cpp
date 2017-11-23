#include "FastRand.h"

#include <outcome.hpp>
#include <benchmark/benchmark.h>
#include <sstream>
#include <string>

namespace outcome = OUTCOME_V2_NAMESPACE;

ATTRIBUTE_NOINLINE
outcome::result<int>
IMPL_SuccessRateOutcome(int successRate) noexcept {
  if (fastrand() % 100 > successRate)
    return std::make_error_code(std::errc::invalid_argument);

  return successRate - fastrand() % 10;
}

void BM_SuccessRateOutcome(benchmark::State &state) {
  int successRate = state.range(0);
  std::ostringstream nulls;

  while (state.KeepRunning()) {
    auto res = IMPL_SuccessRateOutcome(successRate);

    if (!res) {
      nulls << "[OverheadExample] " << res.error();
    }

    benchmark::DoNotOptimize(res);
  }
}

// -----------------------------------------------------------------------------

BENCHMARK(BM_SuccessRateOutcome)->Arg(100)->Arg(95)->Arg(50)->Arg(0);
