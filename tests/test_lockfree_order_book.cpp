/*
 * Lock-Free Order Book Tests
 * Tests correctness, thread safety, and performance
 */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

#include "flox/book/events/book_update_event.h"
#include "flox/book/full_order_book.h"  // For comparison benchmarks
#include "flox/book/lockfree_order_book.h"
#include "flox/engine/abstract_event_pool.h"

using namespace flox;
using namespace std::chrono;

class LockFreeOrderBookTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    tickSize = Price::fromDouble(0.01);
    book = std::make_unique<LockFreeOrderBook<1024>>(tickSize);
  }

  Price tickSize;
  std::unique_ptr<LockFreeOrderBook<1024>> book;

  // Helper to create book update event
  BookUpdateEvent* createUpdate(BookUpdateType type = BookUpdateType::DELTA)
  {
    auto* event = new BookUpdateEvent(std::pmr::get_default_resource());
    event->update.symbol = 0;
    event->update.type = type;
    return event;
  }
};

// Basic functionality tests
TEST_F(LockFreeOrderBookTest, EmptyBook)
{
  EXPECT_FALSE(book->bestBid().has_value());
  EXPECT_FALSE(book->bestAsk().has_value());
  EXPECT_EQ(book->bidAtPrice(Price::fromDouble(100.0)), Quantity{0});
  EXPECT_EQ(book->askAtPrice(Price::fromDouble(100.0)), Quantity{0});
}

TEST_F(LockFreeOrderBookTest, SingleLevelUpdate)
{
  auto* update = createUpdate();
  update->update.bids.emplace_back(Price::fromDouble(99.99), Quantity::fromDouble(100));
  update->update.asks.emplace_back(Price::fromDouble(100.01), Quantity::fromDouble(200));

  book->applyBookUpdate(*update);

  ASSERT_TRUE(book->bestBid().has_value());
  ASSERT_TRUE(book->bestAsk().has_value());
  EXPECT_EQ(book->bestBid().value(), Price::fromDouble(99.99));
  EXPECT_EQ(book->bestAsk().value(), Price::fromDouble(100.01));
  EXPECT_EQ(book->bidAtPrice(Price::fromDouble(99.99)), Quantity::fromDouble(100));
  EXPECT_EQ(book->askAtPrice(Price::fromDouble(100.01)), Quantity::fromDouble(200));

  delete update;
}

TEST_F(LockFreeOrderBookTest, MultiLevelUpdate)
{
  auto* update = createUpdate();

  // Add multiple bid levels
  update->update.bids.emplace_back(Price::fromDouble(99.99), Quantity::fromDouble(100));
  update->update.bids.emplace_back(Price::fromDouble(99.98), Quantity::fromDouble(200));
  update->update.bids.emplace_back(Price::fromDouble(99.97), Quantity::fromDouble(300));

  // Add multiple ask levels
  update->update.asks.emplace_back(Price::fromDouble(100.01), Quantity::fromDouble(150));
  update->update.asks.emplace_back(Price::fromDouble(100.02), Quantity::fromDouble(250));
  update->update.asks.emplace_back(Price::fromDouble(100.03), Quantity::fromDouble(350));

  book->applyBookUpdate(*update);

  // Verify best prices
  EXPECT_EQ(book->bestBid().value(), Price::fromDouble(99.99));
  EXPECT_EQ(book->bestAsk().value(), Price::fromDouble(100.01));

  // Test getTopOfBook
  auto top = book->getTopOfBook<5>();
  EXPECT_EQ(top.bidCount, 3);
  EXPECT_EQ(top.askCount, 3);

  EXPECT_EQ(top.bids[0].first, Price::fromDouble(99.99));
  EXPECT_EQ(top.bids[0].second, Quantity::fromDouble(100));
  EXPECT_EQ(top.bids[1].first, Price::fromDouble(99.98));
  EXPECT_EQ(top.bids[1].second, Quantity::fromDouble(200));

  delete update;
}

TEST_F(LockFreeOrderBookTest, RemoveLevel)
{
  // First add levels
  auto* update1 = createUpdate();
  update1->update.bids.emplace_back(Price::fromDouble(99.99), Quantity::fromDouble(100));
  update1->update.bids.emplace_back(Price::fromDouble(99.98), Quantity::fromDouble(200));
  book->applyBookUpdate(*update1);

  // Remove best bid
  auto* update2 = createUpdate();
  update2->update.bids.emplace_back(Price::fromDouble(99.99), Quantity{0});
  book->applyBookUpdate(*update2);

  // Best bid should now be 99.98
  EXPECT_EQ(book->bestBid().value(), Price::fromDouble(99.98));

  delete update1;
  delete update2;
}

TEST_F(LockFreeOrderBookTest, Snapshot)
{
  // Add some initial data
  auto* update1 = createUpdate();
  update1->update.bids.emplace_back(Price::fromDouble(99.99), Quantity::fromDouble(100));
  book->applyBookUpdate(*update1);

  // Apply snapshot - should clear existing data
  auto* snapshot = createUpdate(BookUpdateType::SNAPSHOT);
  snapshot->update.bids.emplace_back(Price::fromDouble(98.50), Quantity::fromDouble(500));
  snapshot->update.asks.emplace_back(Price::fromDouble(98.60), Quantity::fromDouble(600));
  book->applyBookUpdate(*snapshot);

  // Old data should be gone
  EXPECT_EQ(book->bidAtPrice(Price::fromDouble(99.99)), Quantity{0});

  // New data should be present
  EXPECT_EQ(book->bestBid().value(), Price::fromDouble(98.50));
  EXPECT_EQ(book->bestAsk().value(), Price::fromDouble(98.60));

  delete update1;
  delete snapshot;
}

TEST_F(LockFreeOrderBookTest, SpreadCalculation)
{
  auto* update = createUpdate();
  update->update.bids.emplace_back(Price::fromDouble(99.99), Quantity::fromDouble(100));
  update->update.asks.emplace_back(Price::fromDouble(100.01), Quantity::fromDouble(200));
  book->applyBookUpdate(*update);

  // Spread should be 2 ticks (0.02 / 0.01)
  EXPECT_EQ(book->getSpreadTicks(), 2);

  delete update;
}

// Thread safety tests
TEST_F(LockFreeOrderBookTest, ConcurrentReads)
{
  // Pre-populate book
  auto* update = createUpdate();
  for (int i = 0; i < 100; ++i)
  {
    update->update.bids.emplace_back(
        Price::fromDouble(99.0 - i * 0.01),  // Bids from 99.0 down
        Quantity::fromDouble(100 + i));
    update->update.asks.emplace_back(
        Price::fromDouble(101.0 + i * 0.01),  // Asks from 101.0 up
        Quantity::fromDouble(100 + i));
  }
  book->applyBookUpdate(*update);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> readCount{0};

  // Launch reader threads
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i)
  {
    readers.emplace_back([this, &stop, &readCount]()
                         {
      while (!stop.load())
      {
        auto bid = book->bestBid();
        auto ask = book->bestAsk();
        if (bid.has_value() && ask.has_value())
        {
          EXPECT_LT(bid.value(), ask.value());
        }
        book->getTopOfBook<10>();
        readCount.fetch_add(1);
      } });
  }

  // Let readers run for a bit
  std::this_thread::sleep_for(milliseconds(100));
  stop.store(true);

  for (auto& t : readers)
    t.join();

  // Should have done many reads
  EXPECT_GT(readCount.load(), 10000);

  delete update;
}

TEST_F(LockFreeOrderBookTest, ConcurrentUpdates)
{
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> updateCount{0};
  std::atomic<uint64_t> readCount{0};

  // Writer thread
  std::thread writer([this, &stop, &updateCount]()
                     {
    std::mt19937 gen(42);
    std::uniform_real_distribution<> bidPriceDist(98.0, 100.0);  // Bids below 100
    std::uniform_real_distribution<> askPriceDist(100.0, 102.0);  // Asks above 100
    std::uniform_real_distribution<> qtyDist(0.0, 1000.0);
    
    while (!stop.load())
    {
      auto* update = createUpdate();
      
      // Random updates
      for (int i = 0; i < 5; ++i)
      {
        update->update.bids.emplace_back(
          Price::fromDouble(bidPriceDist(gen)),
          Quantity::fromDouble(qtyDist(gen))
        );
        update->update.asks.emplace_back(
          Price::fromDouble(askPriceDist(gen)),
          Quantity::fromDouble(qtyDist(gen))
        );
      }
      
      book->applyBookUpdate(*update);
      updateCount.fetch_add(1);
      delete update;
    } });

  // Reader threads
  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i)
  {
    readers.emplace_back([this, &stop, &readCount]()
                         {
      while (!stop.load())
      {
        auto bid = book->bestBid();
        auto ask = book->bestAsk();
        
        // Verify consistency when both exist
        if (bid.has_value() && ask.has_value())
        {
          EXPECT_LE(bid.value(), ask.value());
        }
        
        // Get top of book
        auto top = book->getTopOfBook<5>();
        
        // Verify sorted order
        for (size_t i = 1; i < top.bidCount; ++i)
        {
          EXPECT_GE(top.bids[i-1].first, top.bids[i].first);
        }
        for (size_t i = 1; i < top.askCount; ++i)
        {
          EXPECT_LE(top.asks[i-1].first, top.asks[i].first);
        }
        
        readCount.fetch_add(1);
      } });
  }

  // Run test
  std::this_thread::sleep_for(milliseconds(500));
  stop.store(true);

  writer.join();
  for (auto& t : readers)
    t.join();

  // Should have processed many updates
  EXPECT_GT(updateCount.load(), 1000);
  EXPECT_GT(readCount.load(), 10000);
}

// Performance benchmarks
TEST_F(LockFreeOrderBookTest, RealisticProductionScenario)
{
  // Production-like scenario: 1 writer, multiple readers
  const int WARMUP = 1000;
  const int UPDATES = 50000;
  const int NUM_READERS = 3;  // Strategy, Risk, Analytics

  // Create update events
  std::vector<BookUpdateEvent*> updates;
  std::mt19937 gen(42);
  std::uniform_real_distribution<> priceDist(95.0, 105.0);
  std::uniform_real_distribution<> qtyDist(0.0, 1000.0);

  for (int i = 0; i < UPDATES; ++i)
  {
    auto* update = createUpdate();
    // Simulate realistic market data with multiple levels
    int numLevels = (i % 10) + 1;  // 1-10 levels per update
    for (int j = 0; j < numLevels; ++j)
    {
      update->update.bids.emplace_back(
          Price::fromDouble(priceDist(gen)),
          Quantity::fromDouble(qtyDist(gen)));
      update->update.asks.emplace_back(
          Price::fromDouble(priceDist(gen)),
          Quantity::fromDouble(qtyDist(gen)));
    }
    updates.push_back(update);
  }

  // Test lock-free with concurrent readers
  std::atomic<bool> stopReaders{false};
  std::atomic<uint64_t> totalReads{0};

  // Pre-populate
  for (int i = 0; i < WARMUP; ++i)
  {
    book->applyBookUpdate(*updates[i % updates.size()]);
  }

  // Start reader threads (strategy, risk, analytics)
  std::vector<std::thread> readers;

  // Strategy thread - frequent BBO checks
  readers.emplace_back([this, &stopReaders, &totalReads]()
                       {
    while (!stopReaders.load())
    {
      auto bid = book->bestBid();
      auto ask = book->bestAsk();
      if (bid.has_value() && ask.has_value())
      {
        auto spread = ask.value().raw() - bid.value().raw();
        asm volatile("" : : "r"(spread) : "memory");
      }
      totalReads.fetch_add(1);
    } });

  // Risk thread - periodic full book snapshots
  readers.emplace_back([this, &stopReaders, &totalReads]()
                       {
    while (!stopReaders.load())
    {
      auto top = book->getTopOfBook<10>();
      totalReads.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::microseconds(10));  // Less frequent
    } });

  // Analytics thread - various price level checks
  readers.emplace_back([this, &stopReaders, &totalReads]()
                       {
    std::mt19937 gen(123);
    std::uniform_real_distribution<> priceDist(95.0, 105.0);
    while (!stopReaders.load())
    {
      auto price = Price::fromDouble(priceDist(gen));
      auto bidQty = book->bidAtPrice(price);
      auto askQty = book->askAtPrice(price);
      asm volatile("" : : "r,m"(bidQty), "r,m"(askQty) : "memory");
      totalReads.fetch_add(1);
    } });

  // Market data writer thread
  auto lockfreeStart = high_resolution_clock::now();
  for (int i = WARMUP; i < UPDATES; ++i)
  {
    book->applyBookUpdate(*updates[i]);
  }
  auto lockfreeEnd = high_resolution_clock::now();

  stopReaders.store(true);
  for (auto& t : readers)
    t.join();

  auto lockfreeDuration = duration_cast<microseconds>(lockfreeEnd - lockfreeStart);
  double lockfreeUpdatesPerSec = (UPDATES - WARMUP) / (lockfreeDuration.count() / 1e6);
  double lockfreeReadsPerSec = totalReads.load() / (lockfreeDuration.count() / 1e6);

  // Test mutex-based with same scenario
  auto mutexBook = std::make_unique<FullOrderBook>(tickSize);
  stopReaders.store(false);
  totalReads.store(0);

  // Pre-populate
  for (int i = 0; i < WARMUP; ++i)
  {
    mutexBook->applyBookUpdate(*updates[i % updates.size()]);
  }

  // Start reader threads
  readers.clear();

  // Strategy thread
  readers.emplace_back([&mutexBook, &stopReaders, &totalReads]()
                       {
    while (!stopReaders.load())
    {
      auto bid = mutexBook->bestBid();
      auto ask = mutexBook->bestAsk();
      if (bid.has_value() && ask.has_value())
      {
        auto spread = ask.value().raw() - bid.value().raw();
        asm volatile("" : : "r"(spread) : "memory");
      }
      totalReads.fetch_add(1);
    } });

  // Risk thread
  readers.emplace_back([&mutexBook, &stopReaders, &totalReads]()
                       {
    while (!stopReaders.load())
    {
      // Simulate getting top levels (mutex doesn't have getTopOfBook)
      for (int i = 0; i < 5; ++i)
      {
        auto bid = mutexBook->bidAtPrice(Price::fromDouble(100.0 - i * 0.01));
        auto ask = mutexBook->askAtPrice(Price::fromDouble(100.0 + i * 0.01));
        asm volatile("" : : "r,m"(bid), "r,m"(ask) : "memory");
      }
      totalReads.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    } });

  // Analytics thread
  readers.emplace_back([&mutexBook, &stopReaders, &totalReads]()
                       {
    std::mt19937 gen(123);
    std::uniform_real_distribution<> priceDist(95.0, 105.0);
    while (!stopReaders.load())
    {
      auto price = Price::fromDouble(priceDist(gen));
      auto bidQty = mutexBook->bidAtPrice(price);
      auto askQty = mutexBook->askAtPrice(price);
      asm volatile("" : : "r,m"(bidQty), "r,m"(askQty) : "memory");
      totalReads.fetch_add(1);
    } });

  // Market data writer
  auto mutexStart = high_resolution_clock::now();
  for (int i = WARMUP; i < UPDATES; ++i)
  {
    mutexBook->applyBookUpdate(*updates[i]);
  }
  auto mutexEnd = high_resolution_clock::now();

  stopReaders.store(true);
  for (auto& t : readers)
    t.join();

  auto mutexDuration = duration_cast<microseconds>(mutexEnd - mutexStart);
  double mutexUpdatesPerSec = (UPDATES - WARMUP) / (mutexDuration.count() / 1e6);
  double mutexReadsPerSec = totalReads.load() / (mutexDuration.count() / 1e6);

  // Calculate performance ratios for assertions
  double writeImprovement = lockfreeUpdatesPerSec / mutexUpdatesPerSec;
  double readImprovement = lockfreeReadsPerSec / mutexReadsPerSec;
  double totalSpeedup = static_cast<double>(mutexDuration.count()) / lockfreeDuration.count();

  // Cleanup
  for (auto* update : updates)
    delete update;

  // In production scenarios, lock-free should show clear benefits
  EXPECT_GT(lockfreeUpdatesPerSec, mutexUpdatesPerSec);
  EXPECT_GT(lockfreeReadsPerSec, mutexReadsPerSec);
  // Expect significant improvement in total execution time
  EXPECT_GT(totalSpeedup, 1.5);
}

TEST_F(LockFreeOrderBookTest, ConcurrentReadScalability)
{
  // Test how read performance scales with multiple concurrent readers
  // This is critical for production where many threads need market data

  // Pre-populate both books with realistic market depth
  auto* update = createUpdate();
  for (int i = 0; i < 50; ++i)  // Deeper book
  {
    update->update.bids.emplace_back(
        Price::fromDouble(100.0 - i * 0.01),
        Quantity::fromDouble(100 + i * 10));
    update->update.asks.emplace_back(
        Price::fromDouble(100.0 + i * 0.01),
        Quantity::fromDouble(100 + i * 10));
  }

  // Populate lock-free book
  book->applyBookUpdate(*update);

  // Populate mutex-based book
  auto mutexBook = std::make_unique<FullOrderBook>(tickSize);
  mutexBook->applyBookUpdate(*update);

  // Test with different numbers of concurrent readers
  std::vector<int> readerCounts = {1, 2, 4, 8};

  for (int numReaders : readerCounts)
  {
    const int TEST_DURATION_MS = 100;

    // Test lock-free
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> totalReads{0};
    std::vector<std::thread> readers;

    for (int i = 0; i < numReaders; ++i)
    {
      readers.emplace_back([this, &stop, &totalReads]()
                           {
        while (!stop.load())
        {
          auto bid = book->bestBid();
          auto ask = book->bestAsk();
          auto top = book->getTopOfBook<5>();
          totalReads.fetch_add(1);
        } });
    }

    std::this_thread::sleep_for(milliseconds(TEST_DURATION_MS));
    stop.store(true);

    for (auto& t : readers)
      t.join();

    double lockfreeReadsPerSec = totalReads.load() / (TEST_DURATION_MS / 1000.0);

    // Test mutex-based
    stop.store(false);
    totalReads.store(0);
    readers.clear();

    for (int i = 0; i < numReaders; ++i)
    {
      readers.emplace_back([&mutexBook, &stop, &totalReads]()
                           {
        while (!stop.load())
        {
          auto bid = mutexBook->bestBid();
          auto ask = mutexBook->bestAsk();
          // Simulate getTopOfBook
          for (int j = 0; j < 5; ++j)
          {
            mutexBook->bidAtPrice(Price::fromDouble(100.0 - j * 0.01));
            mutexBook->askAtPrice(Price::fromDouble(100.0 + j * 0.01));
          }
          totalReads.fetch_add(1);
        } });
    }

    std::this_thread::sleep_for(milliseconds(TEST_DURATION_MS));
    stop.store(true);

    for (auto& t : readers)
      t.join();

    double mutexReadsPerSec = totalReads.load() / (TEST_DURATION_MS / 1000.0);

    // Scalability should improve with more readers
    if (numReaders > 1)
    {
      EXPECT_GT(lockfreeReadsPerSec / mutexReadsPerSec, 2.0);
    }
  }

  delete update;
}

// Memory footprint test
TEST_F(LockFreeOrderBookTest, MemoryFootprint)
{
  constexpr size_t MAX_LEVELS = 1024;
  size_t expectedSize = sizeof(LockFreeOrderBook<MAX_LEVELS>) +
                        sizeof(std::atomic<Quantity>) * MAX_LEVELS * 2 +
                        (64 - sizeof(std::atomic<Quantity>)) * MAX_LEVELS * 2;  // Padding

  size_t actualSize = LockFreeOrderBook<MAX_LEVELS>::memoryFootprint();

  // Each level should be exactly one cache line
  EXPECT_EQ(actualSize / (MAX_LEVELS * 2), 64);

  // Verify expected size calculation is correct
  EXPECT_LE(actualSize, expectedSize);
}

// Contention performance test
TEST_F(LockFreeOrderBookTest, ContentionPerformanceComparison)
{
  const int NUM_READERS = 3;
  const int UPDATES_PER_WRITER = 10000;

  // Create update events
  std::vector<BookUpdateEvent*> updates;
  std::mt19937 gen(42);
  std::uniform_real_distribution<> priceDist(95.0, 105.0);
  std::uniform_real_distribution<> qtyDist(0.0, 1000.0);

  for (int i = 0; i < UPDATES_PER_WRITER; ++i)
  {
    auto* update = createUpdate();
    update->update.bids.emplace_back(
        Price::fromDouble(priceDist(gen)),
        Quantity::fromDouble(qtyDist(gen)));
    update->update.asks.emplace_back(
        Price::fromDouble(priceDist(gen)),
        Quantity::fromDouble(qtyDist(gen)));
    updates.push_back(update);
  }

  // Pre-populate books
  for (int i = 0; i < 100; ++i)
  {
    book->applyBookUpdate(*updates[i]);
  }

  // Test lock-free under contention
  std::atomic<bool> stopLockfree{false};
  std::atomic<uint64_t> lockfreeReads{0};

  std::vector<std::thread> lockfreeReaders;
  for (int i = 0; i < NUM_READERS; ++i)
  {
    lockfreeReaders.emplace_back([this, &stopLockfree, &lockfreeReads]()
                                 {
      while (!stopLockfree.load())
      {
        auto bid = book->bestBid();
        auto ask = book->bestAsk();
        auto top = book->getTopOfBook<5>();
        lockfreeReads.fetch_add(1);
      } });
  }

  // Writer thread
  auto lockfreeStart = high_resolution_clock::now();
  for (int i = 100; i < UPDATES_PER_WRITER; ++i)
  {
    book->applyBookUpdate(*updates[i]);
  }
  auto lockfreeEnd = high_resolution_clock::now();

  stopLockfree.store(true);
  for (auto& t : lockfreeReaders)
    t.join();

  auto lockfreeDuration = duration_cast<microseconds>(lockfreeEnd - lockfreeStart);

  // Test mutex-based under contention
  auto mutexBook = std::make_unique<FullOrderBook>(tickSize);

  // Pre-populate
  for (int i = 0; i < 100; ++i)
  {
    mutexBook->applyBookUpdate(*updates[i]);
  }

  std::atomic<bool> stopMutex{false};
  std::atomic<uint64_t> mutexReads{0};

  std::vector<std::thread> mutexReaders;
  for (int i = 0; i < NUM_READERS; ++i)
  {
    mutexReaders.emplace_back([&mutexBook, &stopMutex, &mutexReads]()
                              {
      while (!stopMutex.load())
      {
        auto bid = mutexBook->bestBid();
        auto ask = mutexBook->bestAsk();
        // Note: FullOrderBook doesn't have getTopOfBook
        mutexReads.fetch_add(1);
      } });
  }

  // Writer thread
  auto mutexStart = high_resolution_clock::now();
  for (int i = 100; i < UPDATES_PER_WRITER; ++i)
  {
    mutexBook->applyBookUpdate(*updates[i]);
  }
  auto mutexEnd = high_resolution_clock::now();

  stopMutex.store(true);
  for (auto& t : mutexReaders)
    t.join();

  auto mutexDuration = duration_cast<microseconds>(mutexEnd - mutexStart);

  // Calculate reads per second
  double lockfreeReadsPerSec = lockfreeReads.load() / (lockfreeDuration.count() / 1e6);
  double mutexReadsPerSec = mutexReads.load() / (mutexDuration.count() / 1e6);

  // Cleanup
  for (auto* update : updates)
    delete update;

  // Under contention, lock-free should show significant improvement
  EXPECT_LT(lockfreeDuration.count(), mutexDuration.count());
  // Compare reads/sec not total reads (lock-free finishes faster so fewer total reads)
  EXPECT_GT(lockfreeReadsPerSec, mutexReadsPerSec);
  // Expect at least 2x improvement for writes under contention
  EXPECT_GT(static_cast<double>(mutexDuration.count()) / lockfreeDuration.count(), 2.0);
  // Expect significant read throughput improvement
  EXPECT_GT(lockfreeReadsPerSec / mutexReadsPerSec, 2.0);
}