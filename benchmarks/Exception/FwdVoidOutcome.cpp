#include "common/Compiler.h"
#include "common/FastRand.h"

#include <outcome/outcome.hpp>
#include <benchmark/benchmark.h>
#include <string>
#include <sstream>

namespace outcome = OUTCOME_V2_NAMESPACE;

namespace FwdVoidOutcome {

template <int N>
ATTRIBUTE_NOINLINE outcome::result<void, std::string>
IMPL_FwdVoidOutcome(int gt10) noexcept {
  return IMPL_FwdVoidOutcome<N - 1>(gt10);
}

template <>
ATTRIBUTE_NOINLINE outcome::result<void, std::string>
IMPL_FwdVoidOutcome<1>(int gt10) noexcept {
  if (fastrand() % 10 > gt10)
    return "Mocked Error";  // never happens

  return outcome::success();
}

template <int N>
void BM_FwdVoidOutcome(benchmark::State &state) {
  std::ostringstream nulls;

  while (state.KeepRunning()) {
    int gt10 = fastrand() % 10 + 100;
    auto res = IMPL_FwdVoidOutcome<N>(gt10);

    if (!res) {
      nulls << "[never happens]" << res.error();
    }
  }
}

BENCHMARK_TEMPLATE1(BM_FwdVoidOutcome, 1);
BENCHMARK_TEMPLATE1(BM_FwdVoidOutcome, 2);
BENCHMARK_TEMPLATE1(BM_FwdVoidOutcome, 4);
BENCHMARK_TEMPLATE1(BM_FwdVoidOutcome, 8);

}
