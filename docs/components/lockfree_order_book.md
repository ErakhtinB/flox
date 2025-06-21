# Lock-Free Order Book

A high-performance, lock-free order book implementation designed for HFT systems requiring ultra-low latency.

## Overview

The `LockFreeOrderBook` is a specialized implementation of `IOrderBook` that uses atomic operations instead of mutexes, providing:

- **Zero-lock operations** - No mutex contention
- **Cache-line aligned** - Prevents false sharing
- **Fixed memory layout** - Predictable performance
- **Ultra-low latency** - Consistent sub-microsecond operations

## Design Principles

### 1. Lock-Free Architecture

```cpp
// Traditional mutex-based approach (FullOrderBook)
std::mutex _mutex;
void applyUpdate() {
    std::lock_guard lock(_mutex);  // Contention point!
    // ... update logic
}

// Lock-free approach
std::atomic<Quantity> quantity;
void applyUpdate() {
    quantity.store(newQty, std::memory_order_release);  // No locks!
}
```

### 2. Cache-Line Alignment

Each price level occupies exactly one cache line (64 bytes):

```cpp
struct alignas(64) PriceLevel {
    std::atomic<Quantity> quantity{Quantity{0}};
    char _padding[64 - sizeof(std::atomic<Quantity>)];
};
```

This prevents false sharing when multiple threads access different price levels.

### 3. Fixed-Size Arrays

Unlike hash maps, arrays provide:
- O(1) access with predictable latency
- Better cache locality
- No dynamic allocations

## Usage

### Basic Usage

```cpp
#include "flox/book/lockfree_order_book.h"

// Create order book with 4096 price levels
auto book = std::make_unique<LockFreeOrderBook<4096>>(
    Price::fromDouble(0.01)  // tick size
);

// Apply updates
BookUpdateEvent update;
update.update.bids.emplace_back(Price::fromDouble(100.00), Quantity::fromDouble(1000));
update.update.asks.emplace_back(Price::fromDouble(100.01), Quantity::fromDouble(500));
book->applyBookUpdate(update);

// Read best prices
auto bestBid = book->bestBid();  // Ultra-fast atomic read
auto bestAsk = book->bestAsk();  // Ultra-fast atomic read
```

### High-Performance Features

```cpp
// Get top N levels in one call
auto top = book->getTopOfBook<5>();
for (size_t i = 0; i < top.bidCount; ++i) {
    auto [price, qty] = top.bids[i];
    // Process level...
}

// Get spread in ticks
int64_t spreadTicks = book->getSpreadTicks();

// Check quantity at specific price
auto qty = book->bidAtPrice(Price::fromDouble(99.99));
```

## Performance Characteristics

### Architectural Advantages

The lock-free design provides significant performance benefits:

- **Wait-free reads**: Readers never block, even during concurrent updates
- **Lock-free writes**: Updates use atomic operations, eliminating mutex overhead
- **Cache-friendly**: Each price level fits in a single cache line
- **Predictable latency**: No dynamic allocations or hash table lookups
- **Scales with contention**: Performance degrades gracefully under high concurrency

### Relative Performance

| Operation | Comparison to Mutex-Based |
|-----------|--------------------------|
| Best Bid/Ask (no contention) | 3-5x faster |
| Best Bid/Ask (with contention) | 5-10x faster |
| Single Update | 2-3x faster |
| Bulk Updates | 3-5x faster |
| Top N Levels | 4-6x faster |

### Throughput Characteristics

- **Single-threaded**: Millions of updates per second
- **Multi-threaded**: Maintains high throughput with multiple concurrent readers
- **Contention handling**: Performance remains stable under high contention

### Memory Usage

```cpp
// Memory footprint calculation
template<size_t MaxLevels>
size_t memory = sizeof(LockFreeOrderBook<MaxLevels>) +
                (64 bytes × MaxLevels × 2);  // Bids + Asks

// Examples:
// 1024 levels = ~128 KB
// 4096 levels = ~512 KB
// 16384 levels = ~2 MB
```

## Configuration

### Template Parameters

```cpp
template<size_t MaxLevels = 4096>
class LockFreeOrderBook;
```

- `MaxLevels`: Maximum price levels per side
  - Must be power of 2 for optimal performance
  - Common values: 1024, 4096, 16384

### Choosing the Right Size

| Market Type | Recommended Levels | Memory |
|-------------|-------------------|---------|
| Equity | 1024 | 128 KB |
| FX Spot | 2048 | 256 KB |
| Futures | 4096 | 512 KB |
| Crypto | 8192-16384 | 1-2 MB |

## Thread Safety

The lock-free design provides:

- **Wait-free reads**: Readers never block
- **Lock-free writes**: Writers use atomic operations
- **ABA-safe**: Generation counter prevents ABA problems
- **Memory ordering**: Proper acquire-release semantics

## Integration Example

```cpp
// Factory pattern for easy integration
class LockFreeOrderBookFactory : public IOrderBookFactory {
public:
    std::shared_ptr<IOrderBook> create(SymbolId symbol, Price tickSize) override {
        return std::make_shared<LockFreeOrderBook<4096>>(tickSize);
    }
};

// Use with engine
auto engine = EngineBuilder()
    .withOrderBookFactory(std::make_unique<LockFreeOrderBookFactory>())
    .build();
```

## Best Practices

### 1. CPU Affinity

Pin threads to specific cores for best performance:

```cpp
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(core_id, &cpuset);
pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
```

### 2. NUMA Awareness

Allocate order books on the same NUMA node as processing threads:

```cpp
numa_set_preferred(node_id);
auto book = std::make_unique<LockFreeOrderBook<4096>>(tickSize);
```

### 3. Prefetching

The implementation includes prefetch hints:

```cpp
__builtin_prefetch(&_bidLevels[idx], 1, 3);  // Write, high locality
```

### 4. Avoid Cache Line Bouncing

Keep readers and writers on different cores when possible.

## Benchmarking

### Measuring Performance

Performance characteristics vary significantly based on:
- CPU architecture and generation
- Cache hierarchy and sizes
- Memory bandwidth and latency
- Compiler optimizations
- System load and configuration

To measure performance in your environment:

```bash
# Build benchmarks
cmake --build . --target lockfree_order_book_benchmark

# Run benchmarks with custom parameters
./benchmarks/lockfree_order_book_benchmark \
    --iterations=1000000 \
    --threads=4 \
    --warmup=10000
```

### What to Expect

While absolute numbers depend on your hardware, you should observe:
- Consistent sub-microsecond latencies for read operations
- Significant performance improvements under contention
- Linear scaling with CPU frequency
- Stable performance characteristics across different load levels

### Comparing Implementations

The benchmark suite includes comparisons with mutex-based implementations. Focus on:
- Relative performance ratios rather than absolute numbers
- Performance under contention (multiple threads)
- Latency distribution and percentiles
- Worst-case scenarios

## Limitations

1. **Fixed capacity**: Cannot dynamically grow
2. **Memory overhead**: Uses more memory than sparse representations
3. **Price range**: Limited by array size and tick size
4. **No order-level detail**: Only aggregated quantities per price

## Advanced Optimizations

### Custom Memory Allocator

```cpp
// Use huge pages for even better performance
class HugePageAllocator {
    void* allocate(size_t size) {
        return mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    }
};
```

### SIMD Operations

Future versions may include SIMD for:
- Bulk updates
- Finding best prices
- Calculating market metrics

## See Also

- [Abstract Order Book](abstract_order_book.md)
- [Full Order Book](full_order_book.md) - Mutex-based alternative
- [Performance Tuning](../usage/performance.md) 