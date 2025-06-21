/*
 * Flox Engine
 * Developed by Evgenii Makarov (https://github.com/eeiaao)
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include <benchmark/benchmark.h>
#include <memory_resource>
#include <random>
#include <thread>
#include <vector>

#include "flox/book/events/book_update_event.h"
#include "flox/book/full_order_book.h"
#include "flox/book/lockfree_order_book.h"

using namespace flox;

// Test parameters
constexpr size_t NUM_PRICE_LEVELS = 20;
constexpr size_t NUM_UPDATES = 1000;
constexpr size_t NUM_READERS = 3;

// Helper to create realistic book updates
class BookUpdateGenerator
{
 public:
  BookUpdateGenerator(double basePrice = 100.0, double tickSize = 0.01)
      : _gen(std::random_device{}()),
        _basePrice(basePrice),
        _tickSize(tickSize),
        _priceDist(-20, 20),  // +/- 20 ticks from base
        _qtyDist(100, 10000),
        _sideDist(0, 1)
  {
  }

  BookUpdateEvent* generateUpdate(bool snapshot = false)
  {
    auto* event = new BookUpdateEvent(std::pmr::get_default_resource());
    event->update.symbol = 0;
    event->update.type = snapshot ? BookUpdateType::SNAPSHOT : BookUpdateType::DELTA;

    // Generate 1-5 updates per side
    int numUpdates = _updateCountDist(_gen);

    for (int i = 0; i < numUpdates; ++i)
    {
      double price = _basePrice + _priceDist(_gen) * _tickSize;
      double qty = _qtyDist(_gen);

      if (_sideDist(_gen) == 0)
      {
        event->update.bids.emplace_back(
            Price::fromDouble(price),
            Quantity::fromDouble(qty));
      }
      else
      {
        event->update.asks.emplace_back(
            Price::fromDouble(price),
            Quantity::fromDouble(qty));
      }
    }

    return event;
  }

  std::vector<BookUpdateEvent*> generateBatch(size_t count)
  {
    std::vector<BookUpdateEvent*> updates;
    updates.reserve(count);

    // First update is a snapshot
    updates.push_back(generateUpdate(true));

    // Rest are deltas
    for (size_t i = 1; i < count; ++i)
    {
      updates.push_back(generateUpdate(false));
    }

    return updates;
  }

 private:
  std::mt19937 _gen;
  double _basePrice;
  double _tickSize;
  std::uniform_int_distribution<> _priceDist;
  std::uniform_int_distribution<> _qtyDist;
  std::uniform_int_distribution<> _sideDist;
  std::uniform_int_distribution<> _updateCountDist{1, 5};
};

// Benchmark fixtures
class OrderBookBenchmark : public benchmark::Fixture
{
 public:
  void SetUp(const ::benchmark::State& state) override
  {
    tickSize = Price::fromDouble(0.01);
    generator = std::make_unique<BookUpdateGenerator>();
    updates = generator->generateBatch(NUM_UPDATES);
  }

  void TearDown(const ::benchmark::State& state) override
  {
    for (auto* update : updates)
    {
      delete update;
    }
  }

 protected:
  Price tickSize;
  std::unique_ptr<BookUpdateGenerator> generator;
  std::vector<BookUpdateEvent*> updates;
};

// Single-threaded update benchmark
BENCHMARK_DEFINE_F(OrderBookBenchmark, LockFree_Updates)
(benchmark::State& state)
{
  auto book = std::make_unique<LockFreeOrderBook<4096>>(tickSize);

  for (auto _ : state)
  {
    for (auto* update : updates)
    {
      book->applyBookUpdate(*update);
    }
  }

  state.SetItemsProcessed(state.iterations() * updates.size());
}

BENCHMARK_DEFINE_F(OrderBookBenchmark, Mutex_Updates)
(benchmark::State& state)
{
  auto book = std::make_unique<FullOrderBook>(tickSize);

  for (auto _ : state)
  {
    for (auto* update : updates)
    {
      book->applyBookUpdate(*update);
    }
  }

  state.SetItemsProcessed(state.iterations() * updates.size());
}

// Single-threaded read benchmark
BENCHMARK_DEFINE_F(OrderBookBenchmark, LockFree_Reads)
(benchmark::State& state)
{
  auto book = std::make_unique<LockFreeOrderBook<4096>>(tickSize);

  // Pre-populate
  for (size_t i = 0; i < 10; ++i)
  {
    book->applyBookUpdate(*updates[i]);
  }

  for (auto _ : state)
  {
    auto bid = book->bestBid();
    auto ask = book->bestAsk();
    benchmark::DoNotOptimize(bid);
    benchmark::DoNotOptimize(ask);
  }

  state.SetItemsProcessed(state.iterations() * 2);  // 2 reads per iteration
}

BENCHMARK_DEFINE_F(OrderBookBenchmark, Mutex_Reads)
(benchmark::State& state)
{
  auto book = std::make_unique<FullOrderBook>(tickSize);

  // Pre-populate
  for (size_t i = 0; i < 10; ++i)
  {
    book->applyBookUpdate(*updates[i]);
  }

  for (auto _ : state)
  {
    auto bid = book->bestBid();
    auto ask = book->bestAsk();
    benchmark::DoNotOptimize(bid);
    benchmark::DoNotOptimize(ask);
  }

  state.SetItemsProcessed(state.iterations() * 2);
}

// Multi-threaded contention benchmark
void BM_LockFree_Contention(benchmark::State& state)
{
  auto book = std::make_shared<LockFreeOrderBook<4096>>(Price::fromDouble(0.01));
  BookUpdateGenerator generator;
  auto updates = generator.generateBatch(10000);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> readCount{0};

  // Pre-populate
  for (size_t i = 0; i < 100; ++i)
  {
    book->applyBookUpdate(*updates[i]);
  }

  // Start reader threads
  std::vector<std::thread> readers;
  for (size_t i = 0; i < NUM_READERS; ++i)
  {
    readers.emplace_back([&book, &stop, &readCount]()
                         {
      while (!stop.load(std::memory_order_acquire))
      {
        auto bid = book->bestBid();
        auto ask = book->bestAsk();
        auto top = book->getTopOfBook<5>();
        benchmark::DoNotOptimize(bid);
        benchmark::DoNotOptimize(ask);
        benchmark::DoNotOptimize(top);
        readCount.fetch_add(1, std::memory_order_relaxed);
      } });
  }

  // Writer thread (benchmarked)
  size_t updateIdx = 100;
  for (auto _ : state)
  {
    for (size_t i = 0; i < 100; ++i)
    {
      book->applyBookUpdate(*updates[updateIdx % updates.size()]);
      updateIdx++;
    }
  }

  stop.store(true, std::memory_order_release);
  for (auto& t : readers)
  {
    t.join();
  }

  state.SetItemsProcessed(state.iterations() * 100);
  state.counters["ReadThroughput"] = readCount.load();

  // Cleanup
  for (auto* update : updates)
  {
    delete update;
  }
}

void BM_Mutex_Contention(benchmark::State& state)
{
  auto book = std::make_shared<FullOrderBook>(Price::fromDouble(0.01));
  BookUpdateGenerator generator;
  auto updates = generator.generateBatch(10000);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> readCount{0};

  // Pre-populate
  for (size_t i = 0; i < 100; ++i)
  {
    book->applyBookUpdate(*updates[i]);
  }

  // Start reader threads
  std::vector<std::thread> readers;
  for (size_t i = 0; i < NUM_READERS; ++i)
  {
    readers.emplace_back([&book, &stop, &readCount]()
                         {
      while (!stop.load(std::memory_order_acquire))
      {
        auto bid = book->bestBid();
        auto ask = book->bestAsk();
        // Note: FullOrderBook doesn't have getTopOfBook
        benchmark::DoNotOptimize(bid);
        benchmark::DoNotOptimize(ask);
        readCount.fetch_add(1, std::memory_order_relaxed);
      } });
  }

  // Writer thread (benchmarked)
  size_t updateIdx = 100;
  for (auto _ : state)
  {
    for (size_t i = 0; i < 100; ++i)
    {
      book->applyBookUpdate(*updates[updateIdx % updates.size()]);
      updateIdx++;
    }
  }

  stop.store(true, std::memory_order_release);
  for (auto& t : readers)
  {
    t.join();
  }

  state.SetItemsProcessed(state.iterations() * 100);
  state.counters["ReadThroughput"] = readCount.load();

  // Cleanup
  for (auto* update : updates)
  {
    delete update;
  }
}

// Memory footprint comparison
void BM_MemoryFootprint(benchmark::State& state)
{
  size_t levels = state.range(0);

  for (auto _ : state)
  {
    if (levels == 1024)
    {
      size_t lockfreeSize = LockFreeOrderBook<1024>::memoryFootprint();
      benchmark::DoNotOptimize(lockfreeSize);
      state.counters["LockFreeKB"] = lockfreeSize / 1024.0;
    }
    else if (levels == 4096)
    {
      size_t lockfreeSize = LockFreeOrderBook<4096>::memoryFootprint();
      benchmark::DoNotOptimize(lockfreeSize);
      state.counters["LockFreeKB"] = lockfreeSize / 1024.0;
    }

    // Estimate mutex-based size (includes map overhead)
    size_t mutexSize = sizeof(FullOrderBook) +
                       levels * 2 * (sizeof(size_t) + sizeof(Quantity) + 32);  // map overhead
    state.counters["MutexBasedKB"] = mutexSize / 1024.0;
  }
}

// Register benchmarks
BENCHMARK_REGISTER_F(OrderBookBenchmark, LockFree_Updates)->Unit(benchmark::kNanosecond);
BENCHMARK_REGISTER_F(OrderBookBenchmark, Mutex_Updates)->Unit(benchmark::kNanosecond);
BENCHMARK_REGISTER_F(OrderBookBenchmark, LockFree_Reads)->Unit(benchmark::kNanosecond);
BENCHMARK_REGISTER_F(OrderBookBenchmark, Mutex_Reads)->Unit(benchmark::kNanosecond);

BENCHMARK(BM_LockFree_Contention)->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Mutex_Contention)->Unit(benchmark::kMicrosecond)->UseRealTime();

BENCHMARK(BM_MemoryFootprint)->Arg(1024)->Arg(4096);

// Additional latency percentile benchmark
void BM_LatencyPercentiles(benchmark::State& state)
{
  auto book = std::make_unique<LockFreeOrderBook<4096>>(Price::fromDouble(0.01));
  BookUpdateGenerator generator;
  auto updates = generator.generateBatch(10000);

  std::vector<int64_t> latencies;
  latencies.reserve(updates.size());

  for (auto _ : state)
  {
    state.PauseTiming();
    latencies.clear();
    state.ResumeTiming();

    for (auto* update : updates)
    {
      auto start = std::chrono::high_resolution_clock::now();
      book->applyBookUpdate(*update);
      auto end = std::chrono::high_resolution_clock::now();

      auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
      latencies.push_back(latency);
    }

    state.PauseTiming();
    // Calculate percentiles
    std::sort(latencies.begin(), latencies.end());
    size_t p50_idx = latencies.size() * 0.50;
    size_t p95_idx = latencies.size() * 0.95;
    size_t p99_idx = latencies.size() * 0.99;
    size_t p999_idx = latencies.size() * 0.999;

    state.counters["P50_ns"] = latencies[p50_idx];
    state.counters["P95_ns"] = latencies[p95_idx];
    state.counters["P99_ns"] = latencies[p99_idx];
    state.counters["P99.9_ns"] = latencies[p999_idx];
    state.ResumeTiming();
  }

  // Cleanup
  for (auto* update : updates)
  {
    delete update;
  }
}

BENCHMARK(BM_LatencyPercentiles)->Unit(benchmark::kMillisecond)->Iterations(10);

BENCHMARK_MAIN();