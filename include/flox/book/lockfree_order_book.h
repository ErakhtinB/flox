/*
 * Lock-Free Order Book - High Performance Implementation for HFT
 * 
 * Design principles:
 * - Zero locks, only atomic operations
 * - Cache-line aligned data structures
 * - Fixed-size arrays for predictable memory layout
 * - Minimal memory footprint per price level
 * - Fast best bid/ask lookups
 * - Support for millions of updates per second
 */

#pragma once

#include "flox/book/abstract_order_book.h"
#include "flox/book/abstract_order_book_factory.h"
#include "flox/book/events/book_update_event.h"
#include "flox/common.h"

#include <immintrin.h>  // For prefetching
#include <array>
#include <atomic>
#include <cstring>
#include <limits>

namespace flox
{

// Forward declaration for friend class
template <size_t MaxLevels>
class LockFreeOrderBookFactory;

/**
 * Lock-free order book with fixed number of price levels
 * 
 * Memory layout optimized for:
 * - L1/L2 cache efficiency
 * - Predictable memory access patterns
 * - Minimal false sharing between threads
 * 
 * @tparam MaxLevels Maximum number of price levels per side (power of 2 recommended)
 */
template <size_t MaxLevels = 4096>
class LockFreeOrderBook : public IOrderBook
{
  static_assert((MaxLevels & (MaxLevels - 1)) == 0, "MaxLevels should be power of 2 for best performance");

 public:
  explicit LockFreeOrderBook(Price tickSize)
      : _tickSize(tickSize)
  {
    // Initialize all levels to empty
    for (size_t i = 0; i < MaxLevels; ++i)
    {
      _bidLevels[i].quantity.store(Quantity{0}, std::memory_order_relaxed);
      _askLevels[i].quantity.store(Quantity{0}, std::memory_order_relaxed);
    }

    // Initialize best prices to "no market"
    _bestBidIdx.store(INVALID_LEVEL, std::memory_order_relaxed);
    _bestAskIdx.store(INVALID_LEVEL, std::memory_order_relaxed);
  }

  void applyBookUpdate(const BookUpdateEvent& event) override
  {
    const auto& update = event.update;

    // Handle snapshot - increment generation to invalidate old data
    if (update.type == BookUpdateType::SNAPSHOT)
    {
      auto newGen = _generation.fetch_add(1, std::memory_order_acq_rel) + 1;
      clearAllLevels();
    }

    // Prefetch the price levels we're about to update
    if (!update.bids.empty())
    {
      auto idx = priceToIndex(update.bids[0].price);
      if (idx != INVALID_LEVEL && idx < MaxLevels)
        __builtin_prefetch(&_bidLevels[idx], 1, 3);  // Write prefetch, high temporal locality
    }
    if (!update.asks.empty())
    {
      auto idx = priceToIndex(update.asks[0].price);
      if (idx != INVALID_LEVEL && idx < MaxLevels)
        __builtin_prefetch(&_askLevels[idx], 1, 3);
    }

    // Apply bid updates
    bool bidChanged = false;
    size_t minBidIdx = MaxLevels, maxBidIdx = 0;

    for (const auto& level : update.bids)
    {
      size_t idx = priceToIndex(level.price);
      if (idx != INVALID_LEVEL && idx < MaxLevels)
      {
        auto oldQty = _bidLevels[idx].quantity.load(std::memory_order_acquire);
        updateLevel(_bidLevels[idx], level.quantity);

        // Check if this was a removal of the best bid
        if (level.quantity.isZero() && !oldQty.isZero() && idx == _bestBidIdx.load(std::memory_order_acquire))
        {
          bidChanged = true;
        }
        else if (!level.quantity.isZero())
        {
          minBidIdx = std::min(minBidIdx, idx);
          maxBidIdx = std::max(maxBidIdx, idx);
          bidChanged = true;
        }
      }
    }

    // Apply ask updates
    bool askChanged = false;
    size_t minAskIdx = MaxLevels, maxAskIdx = 0;

    for (const auto& level : update.asks)
    {
      size_t idx = priceToIndex(level.price);
      if (idx != INVALID_LEVEL && idx < MaxLevels)
      {
        auto oldQty = _askLevels[idx].quantity.load(std::memory_order_acquire);
        updateLevel(_askLevels[idx], level.quantity);

        // Check if this was a removal of the best ask
        if (level.quantity.isZero() && !oldQty.isZero() && idx == _bestAskIdx.load(std::memory_order_acquire))
        {
          askChanged = true;
        }
        else if (!level.quantity.isZero())
        {
          minAskIdx = std::min(minAskIdx, idx);
          maxAskIdx = std::max(maxAskIdx, idx);
          askChanged = true;
        }
      }
    }

    // Update best bid/ask if needed
    if (bidChanged || update.type == BookUpdateType::SNAPSHOT)
    {
      updateBestBid(minBidIdx, maxBidIdx);
    }
    if (askChanged || update.type == BookUpdateType::SNAPSHOT)
    {
      updateBestAsk(minAskIdx, maxAskIdx);
    }
  }

  std::optional<Price> bestBid() const override
  {
    auto idx = _bestBidIdx.load(std::memory_order_acquire);
    if (idx == INVALID_LEVEL)
      return std::nullopt;
    return indexToPrice(idx);
  }

  std::optional<Price> bestAsk() const override
  {
    auto idx = _bestAskIdx.load(std::memory_order_acquire);
    if (idx == INVALID_LEVEL)
      return std::nullopt;
    return indexToPrice(idx);
  }

  Quantity bidAtPrice(Price price) const override
  {
    size_t idx = priceToIndex(price);
    if (idx == INVALID_LEVEL || idx >= MaxLevels)
      return Quantity{0};

    return _bidLevels[idx].quantity.load(std::memory_order_acquire);
  }

  Quantity askAtPrice(Price price) const override
  {
    size_t idx = priceToIndex(price);
    if (idx == INVALID_LEVEL || idx >= MaxLevels)
      return Quantity{0};

    return _askLevels[idx].quantity.load(std::memory_order_acquire);
  }

  // High-performance method to get top N levels
  template <size_t N>
  struct TopOfBook
  {
    std::array<std::pair<Price, Quantity>, N> bids;
    std::array<std::pair<Price, Quantity>, N> asks;
    size_t bidCount = 0;
    size_t askCount = 0;
  };

  template <size_t N = 5>
  TopOfBook<N> getTopOfBook() const
  {
    TopOfBook<N> result;

    // Get top bids
    auto bidIdx = _bestBidIdx.load(std::memory_order_acquire);
    while (bidIdx != INVALID_LEVEL && result.bidCount < N)
    {
      auto qty = _bidLevels[bidIdx].quantity.load(std::memory_order_acquire);
      if (!qty.isZero())
      {
        result.bids[result.bidCount++] = {indexToPrice(bidIdx), qty};
      }

      // Find next bid level
      bidIdx = findNextBid(bidIdx);
    }

    // Get top asks
    auto askIdx = _bestAskIdx.load(std::memory_order_acquire);
    while (askIdx != INVALID_LEVEL && result.askCount < N)
    {
      auto qty = _askLevels[askIdx].quantity.load(std::memory_order_acquire);
      if (!qty.isZero())
      {
        result.asks[result.askCount++] = {indexToPrice(askIdx), qty};
      }

      // Find next ask level
      askIdx = findNextAsk(askIdx);
    }

    return result;
  }

  // Get spread in ticks
  int64_t getSpreadTicks() const
  {
    auto bidIdx = _bestBidIdx.load(std::memory_order_acquire);
    auto askIdx = _bestAskIdx.load(std::memory_order_acquire);

    if (bidIdx == INVALID_LEVEL || askIdx == INVALID_LEVEL)
      return -1;  // No market

    return static_cast<int64_t>(askIdx) - static_cast<int64_t>(bidIdx);
  }

  // Memory usage estimation
  static constexpr size_t memoryFootprint()
  {
    return sizeof(PriceLevel) * MaxLevels * 2;  // Bids + Asks only (arrays are the main storage)
  }

 private:
  // Price level structure - exactly 64 bytes (1 cache line)
  struct alignas(64) PriceLevel
  {
    std::atomic<Quantity> quantity{Quantity{0}};
    // Padding to fill cache line - prevents false sharing
    char _padding[64 - sizeof(std::atomic<Quantity>)];
  };

  static_assert(sizeof(PriceLevel) == 64, "PriceLevel must be exactly one cache line");

  static constexpr size_t INVALID_LEVEL = std::numeric_limits<size_t>::max();

  // Core data members - each on separate cache line
  alignas(64) Price _tickSize;
  alignas(64) mutable std::atomic<Price> _basePrice{Price{0}};  // Reference price for indexing
  alignas(64) std::atomic<uint32_t> _generation{0};             // For consistent snapshots

  // Best bid/ask indices - hot data, keep on same cache line
  alignas(64) std::atomic<size_t> _bestBidIdx{INVALID_LEVEL};
  alignas(64) std::atomic<size_t> _bestAskIdx{INVALID_LEVEL};

  // Price level arrays - main data storage
  alignas(64) std::array<PriceLevel, MaxLevels> _bidLevels;
  alignas(64) std::array<PriceLevel, MaxLevels> _askLevels;

  // Convert price to array index
  size_t priceToIndex(Price price) const
  {
    // Simple linear mapping - can be optimized based on tick size
    auto basePrice = _basePrice.load(std::memory_order_relaxed);
    if (basePrice.raw() == 0)
    {
      // First price sets the base
      Price expected{0};
      _basePrice.compare_exchange_strong(expected, price);
      return MaxLevels / 2;  // Start in middle
    }

    int64_t offset = (price.raw() - basePrice.raw()) / _tickSize.raw();

    // Early bounds check to prevent overflow
    if (offset < -static_cast<int64_t>(MaxLevels / 2) ||
        offset >= static_cast<int64_t>(MaxLevels / 2))
      return INVALID_LEVEL;

    size_t idx = static_cast<size_t>(offset + MaxLevels / 2);
    return idx;
  }

  // Convert array index back to price
  Price indexToPrice(size_t idx) const
  {
    auto basePrice = _basePrice.load(std::memory_order_relaxed);
    int64_t offset = static_cast<int64_t>(idx) - MaxLevels / 2;
    return Price::fromRaw(basePrice.raw() + offset * _tickSize.raw());
  }

  // Update a single price level
  void updateLevel(PriceLevel& level, Quantity qty)
  {
    level.quantity.store(qty, std::memory_order_release);
  }

  // Clear all levels (for snapshot)
  void clearAllLevels()
  {
    // Clear in chunks to be cache-friendly
    constexpr size_t CHUNK_SIZE = 64;  // Process 64 levels at a time

    for (size_t i = 0; i < MaxLevels; i += CHUNK_SIZE)
    {
      // Prefetch next chunk
      if (i + CHUNK_SIZE < MaxLevels)
      {
        __builtin_prefetch(&_bidLevels[i + CHUNK_SIZE], 1, 0);
        __builtin_prefetch(&_askLevels[i + CHUNK_SIZE], 1, 0);
      }

      // Clear current chunk
      size_t end = std::min(i + CHUNK_SIZE, MaxLevels);
      for (size_t j = i; j < end; ++j)
      {
        _bidLevels[j].quantity.store(Quantity{0}, std::memory_order_relaxed);
        _askLevels[j].quantity.store(Quantity{0}, std::memory_order_relaxed);
      }
    }

    _bestBidIdx.store(INVALID_LEVEL, std::memory_order_release);
    _bestAskIdx.store(INVALID_LEVEL, std::memory_order_release);
  }

  // Find best bid (highest price with quantity)
  void updateBestBid(size_t minHint = 0, size_t maxHint = MaxLevels)
  {
    // If we have hints from the update, use them for a focused search
    if (maxHint < MaxLevels && minHint < MaxLevels)
    {
      // Search from high to low prices within the hint range
      for (size_t i = maxHint + 1; i-- > minHint;)
      {
        if (!_bidLevels[i].quantity.load(std::memory_order_acquire).isZero())
        {
          _bestBidIdx.store(i, std::memory_order_release);
          return;
        }
      }
    }

    // Otherwise do full scan from current best or from top
    size_t startIdx = _bestBidIdx.load(std::memory_order_acquire);
    if (startIdx == INVALID_LEVEL || startIdx >= MaxLevels)
      startIdx = MaxLevels - 1;

    for (size_t i = startIdx + 1; i-- > 0;)
    {
      if (!_bidLevels[i].quantity.load(std::memory_order_acquire).isZero())
      {
        _bestBidIdx.store(i, std::memory_order_release);
        return;
      }
    }
    _bestBidIdx.store(INVALID_LEVEL, std::memory_order_release);
  }

  // Find best ask (lowest price with quantity)
  void updateBestAsk(size_t minHint = 0, size_t maxHint = MaxLevels)
  {
    // If we have hints from the update, use them for a focused search
    if (minHint < MaxLevels && maxHint < MaxLevels)
    {
      // Search from low to high prices within the hint range
      for (size_t i = minHint; i <= maxHint; ++i)
      {
        if (!_askLevels[i].quantity.load(std::memory_order_acquire).isZero())
        {
          _bestAskIdx.store(i, std::memory_order_release);
          return;
        }
      }
    }

    // Otherwise do full scan from current best or from bottom
    size_t startIdx = _bestAskIdx.load(std::memory_order_acquire);
    if (startIdx == INVALID_LEVEL || startIdx >= MaxLevels)
      startIdx = 0;

    for (size_t i = startIdx; i < MaxLevels; ++i)
    {
      if (!_askLevels[i].quantity.load(std::memory_order_acquire).isZero())
      {
        _bestAskIdx.store(i, std::memory_order_release);
        return;
      }
    }
    _bestAskIdx.store(INVALID_LEVEL, std::memory_order_release);
  }

  // Find next best bid (for top of book)
  size_t findNextBid(size_t currentIdx) const
  {
    if (currentIdx == 0)
      return INVALID_LEVEL;

    for (size_t i = currentIdx - 1; i < MaxLevels; --i)
    {
      if (!_bidLevels[i].quantity.load(std::memory_order_acquire).isZero())
        return i;
      if (i == 0)
        break;  // Prevent underflow
    }
    return INVALID_LEVEL;
  }

  // Find next best ask (for top of book)
  size_t findNextAsk(size_t currentIdx) const
  {
    for (size_t i = currentIdx + 1; i < MaxLevels; ++i)
    {
      if (!_askLevels[i].quantity.load(std::memory_order_acquire).isZero())
        return i;
    }
    return INVALID_LEVEL;
  }

  friend class LockFreeOrderBookFactory<MaxLevels>;
};

// Configuration for lock-free order book
struct LockFreeOrderBookConfig : public IOrderBookConfig
{
  Price tickSize{Price::fromDouble(0.01)};
  size_t maxLevels{4096};

  explicit LockFreeOrderBookConfig(Price tick = Price::fromDouble(0.01))
      : tickSize(tick) {}
};

// Factory for creating lock-free order books
template <size_t MaxLevels = 4096>
class LockFreeOrderBookFactory : public IOrderBookFactory
{
 public:
  IOrderBook* create(const IOrderBookConfig& config) override
  {
    auto& lfConfig = static_cast<const LockFreeOrderBookConfig&>(config);
    return new LockFreeOrderBook<MaxLevels>(lfConfig.tickSize);
  }
};

}  // namespace flox