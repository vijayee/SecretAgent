//
// Created by victor on 5/26/25.
//
#include <gmock/gmock.h>
#include <string.h>
#include <atomic>
#include <thread>
#include <vector>
extern "C" {
#include "../src/RefCounter/refcounter.h"
}

TEST(TestRefCounter, TestRefCounterFunctions) {
  refcounter_t* refc1 = (refcounter_t*) calloc(sizeof(refcounter_t), 1);
  refcounter_init(refc1);

  refcounter_t* refc2 = (refcounter_t*) refcounter_reference(refc1);
  EXPECT_EQ(refc1, refc2);

  EXPECT_EQ(refcounter_count(refc1), 2);
  refcounter_yield((refcounter_t*) refc2);
  refc2 = NULL;
  refcounter_t* refc3 = (refcounter_t*) refcounter_reference(refc1);
  EXPECT_EQ(refcounter_count(refc3), 2);
  EXPECT_EQ(refc1, refc3);
  refcounter_dereference(refc3);
  refc3 = NULL;
  EXPECT_EQ(refcounter_count(refc1), 1);
  refcounter_destroy_lock(refc1);
  free(refc1);
}

// Reference helpers for the packed-field layout. These MUST match the
// layout defined in refcounter.h (count:16, yield:8, pending:8).
static uint16_t rc_count(uint32_t state) { return (uint16_t)(state & 0xFFFFu); }
static uint8_t  rc_yield(uint32_t state) { return (uint8_t)((state >> 16) & 0xFFu); }
static uint8_t  rc_pending(uint32_t state) { return (uint8_t)((state >> 24) & 0xFFu); }

TEST(RefCounterEscrow, DoubleAdoptDoesNotUnderflowYield) {
  // yield=1, two threads both reference concurrently. Pre-fix this races:
  // both load yield=1, both fetch_sub, yield wraps to 0xFF and count
  // under-counts. Post-fix, only one adopter consumes the yield slot.
  refcounter_t* refc = (refcounter_t*)calloc(1, sizeof(refcounter_t));
  ASSERT_NE(refc, nullptr);
  refcounter_init(refc);  // count=1, yield=0, pending=0
  refcounter_yield(refc); // yield=1

  std::thread left([&](){ refcounter_reference(refc); });
  std::thread right([&](){ refcounter_reference(refc); });
  left.join();
  right.join();

  // Exactly one adopter should have consumed the yield. count must be 2
  // (the original 1 + one adopter that fell through to count++ because the
  // other took the yield), and yield must be 0 (no underflow to 0xFF).
  uint32_t state = refc->packed_state.load(std::memory_order_relaxed);
  EXPECT_EQ(rc_yield(state), (uint8_t)0) << "yield must not underflow to 0xFF";
  EXPECT_EQ(rc_count(state), (uint16_t)2);
  refcounter_destroy_lock(refc);
  free(refc);
}

TEST(RefCounterEscrow, PendingDerefIsNotStranded) {
  // yield=1; one thread dereferences (should bump pending_deref), another
  // concurrently references (should consume the yield AND the pending_deref,
  // decrementing count). Pre-fix, the adopting reference can check pending
  // before the dereference's ++ lands -> pending is stranded at 1 and the
  // count is off by one. Post-fix, the CAS sees both fields consistently.
  refcounter_t* refc = (refcounter_t*)calloc(1, sizeof(refcounter_t));
  ASSERT_NE(refc, nullptr);
  refcounter_init(refc);     // count=1
  refcounter_reference(refc); // count=2
  refcounter_yield(refc);    // yield=1

  std::thread deref([&](){ refcounter_dereference(refc); });
  std::thread ref([&](){ refcounter_reference(refc); });
  deref.join();
  ref.join();

  // The yield+pending transfer must complete cleanly. Either order produces
  // the same final state:
  //  - ref first: ref adopts yield (count unchanged at 2, yield=0); the
  //    subsequent deref sees yield=0 and does count-- -> count=1.
  //  - deref first: deref bumps pending (count=2, yield=1, pending=1); the
  //    ref then adopts yield AND consumes the pending (count-- -> 1,
  //    yield=0, pending=0).
  // Both paths land at count=1, yield=0, pending=0. The pre-fix race
  // stranded the pending (count=2, pending=1) or under-applied the deref
  // (count=2, yield=0). The packed CAS observes both fields consistently.
  uint32_t state = refc->packed_state.load(std::memory_order_relaxed);
  EXPECT_EQ(rc_yield(state), (uint8_t)0);
  EXPECT_EQ(rc_pending(state), (uint8_t)0);
  EXPECT_EQ(rc_count(state), (uint16_t)1);
  refcounter_destroy_lock(refc);
  free(refc);
}

TEST(RefCounterEscrow, HighConcurrencyAdoptReleaseNoCorruption) {
  // Stress test: many threads reference + dereference a single object with
  // a yield open. Pre-fix, this corrupts count/yield within a few iterations.
  refcounter_t* refc = (refcounter_t*)calloc(1, sizeof(refcounter_t));
  ASSERT_NE(refc, nullptr);
  refcounter_init(refc);
  refcounter_yield(refc);

  std::vector<std::thread> threads;
  for (int thread_index = 0; thread_index < 8; thread_index++) {
    threads.emplace_back([&]() {
      for (int iteration = 0; iteration < 1000; iteration++) {
        refcounter_reference(refc);
        refcounter_dereference(refc);
      }
    });
  }
  for (auto& thread : threads) thread.join();

  // One yield slot, many concurrent adopters: at most one adoption consumes
  // the yield; the rest fall through to count++. Each thread's first op is
  // a ref, so the very first ref across all threads adopts the yield; every
  // later op (refs and derefs alike) sees yield=0. The 1 adopting ref does
  // not change count, the remaining 7999 refs do count++ (+7999), and all
  // 8000 derefs do count-- (-8000). Net: 1 (init) + 0 (adopt) + 7999 - 8000
  // = 0. The yield must be 0 (not 0xFF underflow) and count must be 0 (not
  // corrupted). Pre-fix, yield underflowed to 0xFF and count drifted.
  uint32_t state = refc->packed_state.load(std::memory_order_relaxed);
  EXPECT_EQ(rc_yield(state), (uint8_t)0);
  EXPECT_EQ(rc_count(state), (uint16_t)0);
  refcounter_destroy_lock(refc);
  free(refc);
}