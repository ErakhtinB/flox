# Error Handling System

The Flox engine features a comprehensive error handling system based on C++23's `std::expected`, providing type-safe error propagation without the overhead of exceptions in hot trading paths.

## Table of Contents

1. [Core Components](#core-components)
2. [Error Types and Categories](#error-types-and-categories)
3. [Usage Patterns](#usage-patterns)
4. [Performance Considerations](#performance-considerations)
5. [Thread Safety](#thread-safety)
6. [Best Practices](#best-practices)
7. [Migration Guide](#migration-guide)

## Core Components

### FloxError Class

The `FloxError` class is the foundation of the error system, providing:

- **Error Code**: Specific error identifier from the `ErrorCode` enum
- **Message**: Human-readable error description
- **Severity**: Error severity level (INFO, WARNING, ERROR, CRITICAL, FATAL)
- **Source Location**: Automatic capture of file, line, and function
- **Timestamp**: When the error occurred
- **Context Chaining**: Ability to add context to errors

```cpp
auto error = FLOX_ERROR(ORDER_INVALID_QUANTITY, "Quantity must be positive");
auto contextError = error.withContext("While processing market order");
```

### Result<T> Types

Type-safe error handling using `std::expected`:

```cpp
// Basic result type
Result<int> calculatePosition(const Order& order);

// Void result for operations that don't return values
VoidResult validateOrder(const Order& order);

// Fast result for hot paths (minimal allocation)
FastResult<Price> getCurrentPrice(SymbolId symbol);
```

### Error Categories and Codes

The system defines comprehensive error categories:

- **ENGINE** (1-99): Core engine errors
- **MARKET_DATA** (100-199): Market data processing errors
- **ORDER_EXECUTION** (200-299): Order management errors
- **RISK_MANAGEMENT** (300-399): Risk limit violations
- **CONNECTIVITY** (400-499): Network and connection errors
- **VALIDATION** (500-599): Input validation errors
- **POSITION** (600-699): Position management errors
- **MEMORY** (700-799): Memory allocation errors
- **THREADING** (800-899): Concurrency errors
- **CONFIGURATION** (900-999): Configuration errors

## Usage Patterns

### Basic Error Handling

```cpp
Result<Quantity> getPosition(SymbolId symbol) {
    if (!isValidSymbol(symbol)) {
        return std::unexpected(FLOX_ERROR(POSITION_INVALID_SYMBOL, 
                                         "Invalid symbol ID"));
    }
    
    return _positions[symbol];
}

// Usage
auto result = getPosition(symbolId);
if (result) {
    std::cout << "Position: " << *result << std::endl;
} else {
    std::cerr << "Error: " << result.error().toString() << std::endl;
}
```

### Error Chaining with andThen

```cpp
auto result = getPosition(symbol)
    .and_then([](Quantity pos) -> Result<Quantity> {
        return pos + Quantity::fromDouble(100.0);
    })
    .and_then([](Quantity newPos) -> VoidResult {
        return validatePositionLimits(newPos);
    });
```

### Error Recovery with orElse

```cpp
auto result = connectToPrimaryExchange()
    .or_else([](const FloxError& error) -> Result<Connection> {
        // Fallback to secondary exchange
        return connectToSecondaryExchange();
    });
```

### Error Aggregation

```cpp
ErrorCollector collector;

for (const auto& order : orders) {
    auto result = processOrder(order);
    if (!result) {
        collector.add(result.error());
    }
}

if (collector.hasErrors()) {
    return collector.finalize();  // Returns aggregated error
}
```

## Performance Considerations

### Hot Path Optimization

For high-frequency operations, use `FastResult<T>` which only stores error codes:

```cpp
FastResult<Price> getLastPrice(SymbolId symbol) {
    if (!isValidSymbol(symbol)) {
        return std::unexpected(ErrorCode::POSITION_INVALID_SYMBOL);
    }
    return _lastPrices[symbol];
}

// Convert to full error when needed
if (!result) {
    FloxError fullError = expandError(result.error(), "Price lookup failed");
}
```

### Lock-Free Error Statistics

For monitoring without performance impact:

```cpp
// Thread-safe error reporting
reportErrorFast(ErrorCode::ORDER_INVALID_QUANTITY);

// Get statistics snapshot
auto stats = g_globalErrorStats.getSnapshot();
std::cout << "Total errors: " << stats.totalErrors << std::endl;
```

## Thread Safety

### Error Statistics

- Global error statistics use atomic operations
- Thread-local statistics for minimal contention
- Lock-free snapshot functionality

```cpp
// Thread-safe error recording
void recordError(ErrorCode code, ErrorSeverity severity) noexcept {
    g_threadLocalErrorStats.recordError(code, severity);
    g_globalErrorStats.recordError(code, severity);
}
```

### Position Manager

The enhanced PositionManager uses proper locking hierarchy:

```cpp
// Locking hierarchy for different data types
std::shared_mutex _positionMutex;     // Position data (read-write)
std::mutex _historyMutex;             // History tracking (exclusive)
std::mutex _statsMutex;               // Statistics (exclusive)
```

## Best Practices

### Error Policy Guidelines

1. **Use Result<T> for recoverable errors**
   ```cpp
   Result<Order> parseOrder(const std::string& data);
   ```

2. **Use FastResult<T> for hot paths**
   ```cpp
   FastResult<Price> getMarketPrice(SymbolId symbol);
   ```

3. **Use assertions for programming errors**
   ```cpp
   FLOX_ASSERT(symbol < MAX_SYMBOLS, "Symbol index out of bounds");
   ```

4. **Use contracts for API boundaries**
   ```cpp
   VoidResult processOrder(const Order& order) {
       FLOX_PRECONDITION(!order.quantity.isZero(), "Order quantity cannot be zero");
       // ... implementation
       FLOX_POSTCONDITION(isOrderValid(order), "Order must be valid after processing");
   }
   ```

### Error Context

Always provide meaningful context:

```cpp
auto result = validateOrder(order);
if (!result) {
    return result.error().withContext("During order submission");
}
```

### Severity Guidelines

- **INFO**: Informational messages, system continues normally
- **WARNING**: Potential issues, system continues with degraded performance
- **ERROR**: Operation failed, but system continues
- **CRITICAL**: System functionality impaired, requires attention
- **FATAL**: System must shut down, unrecoverable error

## Migration Guide

### From Exceptions to Results

**Before:**
```cpp
Quantity getPosition(SymbolId symbol) {
    if (!isValidSymbol(symbol)) {
        throw std::invalid_argument("Invalid symbol");
    }
    return _positions[symbol];
}
```

**After:**
```cpp
Result<Quantity> getPosition(SymbolId symbol) {
    if (!isValidSymbol(symbol)) {
        return std::unexpected(FLOX_ERROR(POSITION_INVALID_SYMBOL, 
                                         "Invalid symbol ID"));
    }
    return _positions[symbol];
}
```

### Updating Callers

**Before:**
```cpp
try {
    auto position = getPosition(symbol);
    processPosition(position);
} catch (const std::exception& e) {
    logError(e.what());
}
```

**After:**
```cpp
auto result = getPosition(symbol);
if (result) {
    processPosition(*result);
} else {
    logError(result.error().toString());
}
```

### Error Aggregation Migration

**Before:**
```cpp
std::vector<std::string> errors;
for (const auto& order : orders) {
    try {
        processOrder(order);
    } catch (const std::exception& e) {
        errors.push_back(e.what());
    }
}
```

**After:**
```cpp
ErrorCollector collector;
for (const auto& order : orders) {
    auto result = processOrder(order);
    if (!result) {
        collector.add(result.error());
    }
}

if (collector.hasErrors()) {
    auto aggregatedError = collector.finalize();
    // Handle aggregated error
}
```

## Example: Complete Error-Aware Component

```cpp
class TradingStrategy {
public:
    VoidResult initialize(const Config& config) {
        FLOX_PRECONDITION(!config.symbols.empty(), "Strategy requires symbols");
        
        ErrorCollector collector;
        
        for (const auto& symbol : config.symbols) {
            auto result = validateSymbol(symbol);
            if (!result) {
                collector.add(result.error().withContext("Symbol validation"));
            }
        }
        
        if (collector.hasErrors()) {
            return collector.finalize();
        }
        
        _initialized = true;
        FLOX_POSTCONDITION(_initialized, "Strategy must be initialized");
        return {};
    }
    
    Result<Order> generateOrder(SymbolId symbol, const MarketData& data) {
        if (!_initialized) {
            return std::unexpected(FLOX_ERROR(ENGINE_NOT_STARTED, 
                                             "Strategy not initialized"));
        }
        
        return calculateOrder(symbol, data)
            .and_then([this](Order order) -> Result<Order> {
                return validateOrderRisk(order);
            })
            .and_then([](Order order) -> Result<Order> {
                return finalizeOrder(order);
            });
    }

private:
    bool _initialized = false;
    
    FastResult<Order> calculateOrder(SymbolId symbol, const MarketData& data) {
        // Hot path calculation with minimal error overhead
        if (!isValidMarketData(data)) {
            return std::unexpected(ErrorCode::MARKET_DATA_INVALID_SYMBOL);
        }
        
        // ... calculation logic
        return order;
    }
};
```

This enhanced error handling system provides:

- **Type Safety**: Compile-time error handling verification
- **Performance**: Optimized for high-frequency trading scenarios
- **Reliability**: Comprehensive error tracking and reporting
- **Maintainability**: Clear error propagation and context
- **Debugging**: Rich error information with source location tracking 