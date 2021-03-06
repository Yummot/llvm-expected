#include "common/Compiler.h"
#include "common/FastRand.h"

#include <benchmark/benchmark.h>
#include <sstream>
#include <stdexcept>

namespace FwdException {

template <int N>
ATTRIBUTE_NOINLINE void IMPL_FwdException(int gt10) {
  return IMPL_FwdException<N - 1>(gt10);
}

template <>
ATTRIBUTE_NOINLINE void IMPL_FwdException<1>(int gt10) {
  if (fastrand() % 10 > gt10)
    throw std::runtime_error("some detail"); // never happens
}

template <int N>
void BM_FwdException(benchmark::State &state) {
  std::ostringstream nulls;

  while (state.KeepRunning()) {
    try {
      int gt10 = fastrand() % 10 + 100;
      IMPL_FwdException<N>(gt10);
    } catch (std::runtime_error e) {
      nulls << "[OverheadExample] " << e.what() << "\n";
    }
  }
}

// -----------------------------------------------------------------------------

BENCHMARK_TEMPLATE1(BM_FwdException, 1);
BENCHMARK_TEMPLATE1(BM_FwdException, 2);
BENCHMARK_TEMPLATE1(BM_FwdException, 4);
BENCHMARK_TEMPLATE1(BM_FwdException, 8);

}
