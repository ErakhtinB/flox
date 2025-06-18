/*
 * Flox Engine - Error Handling System
 * Developed by Evgenii Makarov (https://github.com/eeiaao)
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <expected>
#include <format>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <source_location>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace flox
{

// Error categories for different system components
enum class ErrorCategory : uint8_t
{
  ENGINE = 1,
  MARKET_DATA = 2,
  ORDER_EXECUTION = 3,
  RISK_MANAGEMENT = 4,
  CONNECTIVITY = 5,
  VALIDATION = 6,
  POSITION = 7,
  MEMORY = 8,
  THREADING = 9,
  CONFIGURATION = 10
};

// Specific error codes within each category
enum class ErrorCode : uint16_t
{
  // Engine errors (1-99)
  ENGINE_NOT_STARTED = 1,
  ENGINE_ALREADY_RUNNING = 2,
  ENGINE_SHUTDOWN_FAILED = 3,
  SUBSYSTEM_INIT_FAILED = 4,

  // Market Data errors (100-199)
  MARKET_DATA_QUEUE_FULL = 100,
  MARKET_DATA_INVALID_SYMBOL = 101,
  MARKET_DATA_STALE_DATA = 102,
  MARKET_DATA_PARSE_ERROR = 103,
  MARKET_DATA_SUBSCRIPTION_FAILED = 104,

  // Order Execution errors (200-299)
  ORDER_INVALID_QUANTITY = 200,
  ORDER_INVALID_PRICE = 201,
  ORDER_REJECTED_BY_EXCHANGE = 202,
  ORDER_EXECUTION_TIMEOUT = 203,
  ORDER_NOT_FOUND = 204,
  ORDER_ALREADY_FILLED = 205,
  ORDER_CANCEL_REJECTED = 206,

  // Risk Management errors (300-399)
  RISK_POSITION_LIMIT_EXCEEDED = 300,
  RISK_ORDER_SIZE_LIMIT_EXCEEDED = 301,
  RISK_DAILY_LOSS_LIMIT_EXCEEDED = 302,
  RISK_CONCENTRATION_LIMIT_EXCEEDED = 303,
  RISK_LEVERAGE_LIMIT_EXCEEDED = 304,

  // Connectivity errors (400-499)
  CONNECTION_LOST = 400,
  CONNECTION_TIMEOUT = 401,
  CONNECTION_AUTH_FAILED = 402,
  CONNECTION_RATE_LIMITED = 403,
  CONNECTION_PROTOCOL_ERROR = 404,

  // Validation errors (500-599)
  VALIDATION_REQUIRED_FIELD_MISSING = 500,
  VALIDATION_INVALID_FORMAT = 501,
  VALIDATION_OUT_OF_RANGE = 502,
  VALIDATION_DUPLICATE_ENTRY = 503,

  // Position errors (600-699)
  POSITION_INSUFFICIENT_BALANCE = 600,
  POSITION_INVALID_SYMBOL = 601,
  POSITION_UPDATE_CONFLICT = 602,

  // Memory errors (700-799)
  MEMORY_POOL_EXHAUSTED = 700,
  MEMORY_ALLOCATION_FAILED = 701,
  MEMORY_QUEUE_OVERFLOW = 702,

  // Threading errors (800-899)
  THREAD_CREATION_FAILED = 800,
  THREAD_JOIN_FAILED = 801,
  THREAD_SYNCHRONIZATION_ERROR = 802,

  // Configuration errors (900-999)
  CONFIG_FILE_NOT_FOUND = 900,
  CONFIG_PARSE_ERROR = 901,
  CONFIG_VALIDATION_FAILED = 902,
  CONFIG_REQUIRED_PARAMETER_MISSING = 903
};

// Severity levels for errors
enum class ErrorSeverity : uint8_t
{
  INFO = 1,      // Informational, system continues normally
  WARNING = 2,   // Warning, system continues but may be degraded
  ERROR = 3,     // Error, operation failed but system continues
  CRITICAL = 4,  // Critical error, system functionality impaired
  FATAL = 5      // Fatal error, system must shut down
};

// Main error class
class FloxError
{
 public:
  FloxError(ErrorCode code,
            std::string message,
            ErrorSeverity severity = ErrorSeverity::ERROR,
            std::source_location location = std::source_location::current())
      : _code(code), _message(std::move(message)), _severity(severity), _location(location), _timestamp(std::chrono::steady_clock::now())
  {
  }

  // Convenience constructors for different severities
  static FloxError Info(ErrorCode code, std::string message,
                        std::source_location location = std::source_location::current())
  {
    return FloxError(code, std::move(message), ErrorSeverity::INFO, location);
  }

  static FloxError Warning(ErrorCode code, std::string message,
                           std::source_location location = std::source_location::current())
  {
    return FloxError(code, std::move(message), ErrorSeverity::WARNING, location);
  }

  static FloxError Critical(ErrorCode code, std::string message,
                            std::source_location location = std::source_location::current())
  {
    return FloxError(code, std::move(message), ErrorSeverity::CRITICAL, location);
  }

  static FloxError Fatal(ErrorCode code, std::string message,
                         std::source_location location = std::source_location::current())
  {
    return FloxError(code, std::move(message), ErrorSeverity::FATAL, location);
  }

  // Accessors
  ErrorCode code() const noexcept { return _code; }
  const std::string& message() const noexcept { return _message; }
  ErrorSeverity severity() const noexcept { return _severity; }
  const std::source_location& location() const noexcept { return _location; }
  std::chrono::steady_clock::time_point timestamp() const noexcept { return _timestamp; }

  ErrorCategory category() const noexcept
  {
    uint16_t code_val = static_cast<uint16_t>(_code);
    if (code_val < 100)
      return ErrorCategory::ENGINE;
    if (code_val < 200)
      return ErrorCategory::MARKET_DATA;
    if (code_val < 300)
      return ErrorCategory::ORDER_EXECUTION;
    if (code_val < 400)
      return ErrorCategory::RISK_MANAGEMENT;
    if (code_val < 500)
      return ErrorCategory::CONNECTIVITY;
    if (code_val < 600)
      return ErrorCategory::VALIDATION;
    if (code_val < 700)
      return ErrorCategory::POSITION;
    if (code_val < 800)
      return ErrorCategory::MEMORY;
    if (code_val < 900)
      return ErrorCategory::THREADING;
    return ErrorCategory::CONFIGURATION;
  }

  // Formatted error description
  std::string toString() const
  {
    return std::format("[{}] {} at {}:{} - {}",
                       severityToString(_severity),
                       categoryToString(category()),
                       _location.file_name(),
                       _location.line(),
                       _message);
  }

  // Chain errors for context
  FloxError withContext(std::string context) const
  {
    return FloxError(_code,
                     std::format("{}: {}", context, _message),
                     _severity,
                     _location);
  }

 private:
  ErrorCode _code;
  std::string _message;
  ErrorSeverity _severity;
  std::source_location _location;
  std::chrono::steady_clock::time_point _timestamp;

  static constexpr std::string_view severityToString(ErrorSeverity severity)
  {
    switch (severity)
    {
      case ErrorSeverity::INFO:
        return "INFO";
      case ErrorSeverity::WARNING:
        return "WARN";
      case ErrorSeverity::ERROR:
        return "ERROR";
      case ErrorSeverity::CRITICAL:
        return "CRITICAL";
      case ErrorSeverity::FATAL:
        return "FATAL";
    }
    return "UNKNOWN";
  }

  static constexpr std::string_view categoryToString(ErrorCategory category)
  {
    switch (category)
    {
      case ErrorCategory::ENGINE:
        return "ENGINE";
      case ErrorCategory::MARKET_DATA:
        return "MARKET_DATA";
      case ErrorCategory::ORDER_EXECUTION:
        return "ORDER_EXECUTION";
      case ErrorCategory::RISK_MANAGEMENT:
        return "RISK_MANAGEMENT";
      case ErrorCategory::CONNECTIVITY:
        return "CONNECTIVITY";
      case ErrorCategory::VALIDATION:
        return "VALIDATION";
      case ErrorCategory::POSITION:
        return "POSITION";
      case ErrorCategory::MEMORY:
        return "MEMORY";
      case ErrorCategory::THREADING:
        return "THREADING";
      case ErrorCategory::CONFIGURATION:
        return "CONFIGURATION";
    }
    return "UNKNOWN";
  }
};

// Result type alias for cleaner API
template <typename T>
using Result = std::expected<T, FloxError>;

// Void result for operations that don't return values
using VoidResult = Result<void>;

// Helper macros for creating errors with automatic source location
#define FLOX_ERROR(code, message) \
  flox::FloxError(flox::ErrorCode::code, message)

#define FLOX_ERROR_INFO(code, message) \
  flox::FloxError::Info(flox::ErrorCode::code, message)

#define FLOX_ERROR_WARNING(code, message) \
  flox::FloxError::Warning(flox::ErrorCode::code, message)

#define FLOX_ERROR_CRITICAL(code, message) \
  flox::FloxError::Critical(flox::ErrorCode::code, message)

#define FLOX_ERROR_FATAL(code, message) \
  flox::FloxError::Fatal(flox::ErrorCode::code, message)

// Helper functions for common patterns
template <typename T>
constexpr bool isSuccess(const Result<T>& result) noexcept
{
  return result.has_value();
}

template <typename T>
constexpr bool isError(const Result<T>& result) noexcept
{
  return !result.has_value();
}

// Error propagation helpers
template <typename T, typename F>
auto andThen(Result<T>&& result, F&& func) -> std::invoke_result_t<F, T&&>
{
  if (result)
  {
    return func(std::move(*result));
  }

  using ReturnType = std::invoke_result_t<F, T&&>;
  return ReturnType{std::unexpected(std::move(result.error()))};
}

template <typename T, typename F>
auto orElse(Result<T>&& result, F&& func) -> Result<T>
{
  if (result)
  {
    return result;
  }
  return func(std::move(result.error()));
}

// Transform error type while preserving value
template <typename T, typename F>
auto mapError(Result<T>&& result, F&& func) -> Result<T>
{
  if (result)
  {
    return result;
  }
  return std::unexpected(func(std::move(result.error())));
}

// Concepts for error-aware types
template <typename T>
concept Resultable = requires(T t) {
  typename T::value_type;
  typename T::error_type;
  { t.has_value() } -> std::convertible_to<bool>;
  { t.error() } -> std::convertible_to<typename T::error_type>;
};

// Fast error type for hot paths with minimal allocation
template <typename T>
using FastResult = std::expected<T, ErrorCode>;

// Convert ErrorCode to full FloxError when needed
inline FloxError expandError(ErrorCode code, std::string message = "",
                             std::source_location location = std::source_location::current())
{
  if (message.empty())
  {
    message = std::format("Error code: {}", static_cast<uint16_t>(code));
  }
  return FloxError(code, std::move(message), ErrorSeverity::ERROR, location);
}

// Error aggregation for batch operations
class ErrorCollector
{
 public:
  void add(FloxError error)
  {
    _errors.push_back(std::move(error));
  }

  void add(ErrorCode code, std::string message)
  {
    _errors.emplace_back(code, std::move(message));
  }

  bool hasErrors() const noexcept { return !_errors.empty(); }
  size_t count() const noexcept { return _errors.size(); }

  const std::vector<FloxError>& getErrors() const noexcept { return _errors; }

  VoidResult finalize() const
  {
    if (_errors.empty())
    {
      return {};
    }

    if (_errors.size() == 1)
    {
      return std::unexpected(_errors[0]);
    }

    // Aggregate multiple errors into one
    std::string aggregatedMessage = std::format("Multiple errors occurred ({} total):\n", _errors.size());
    for (size_t i = 0; i < _errors.size() && i < 5; ++i)  // Limit to first 5 errors
    {
      aggregatedMessage += std::format("  {}: {}\n", i + 1, _errors[i].message());
    }

    if (_errors.size() > 5)
    {
      aggregatedMessage += std::format("  ... and {} more errors\n", _errors.size() - 5);
    }

    // Use the highest severity error's category and code
    auto maxSeverityIt = std::max_element(_errors.begin(), _errors.end(),
                                          [](const FloxError& a, const FloxError& b)
                                          {
                                            return static_cast<uint8_t>(a.severity()) < static_cast<uint8_t>(b.severity());
                                          });

    return std::unexpected(FloxError(maxSeverityIt->code(), aggregatedMessage, maxSeverityIt->severity()));
  }

 private:
  std::vector<FloxError> _errors;
};

// Lock-free error statistics for hot paths
struct alignas(64) ErrorStats  // Cache line aligned
{
  std::atomic<uint64_t> totalErrors{0};
  std::atomic<uint64_t> criticalErrors{0};
  std::atomic<uint64_t> fatalErrors{0};
  std::atomic<uint64_t> lastErrorTimestamp{0};  // Nanoseconds since epoch
  std::atomic<uint16_t> lastErrorCode{0};

  // Allow resetting the stats
  void reset() noexcept
  {
    totalErrors.store(0, std::memory_order_relaxed);
    criticalErrors.store(0, std::memory_order_relaxed);
    fatalErrors.store(0, std::memory_order_relaxed);
    lastErrorTimestamp.store(0, std::memory_order_relaxed);
    lastErrorCode.store(0, std::memory_order_relaxed);
  }

  void recordError(ErrorCode code, ErrorSeverity severity) noexcept
  {
    totalErrors.fetch_add(1, std::memory_order_relaxed);
    lastErrorCode.store(static_cast<uint16_t>(code), std::memory_order_relaxed);
    lastErrorTimestamp.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);

    if (severity >= ErrorSeverity::CRITICAL)
    {
      criticalErrors.fetch_add(1, std::memory_order_relaxed);
    }
    if (severity == ErrorSeverity::FATAL)
    {
      fatalErrors.fetch_add(1, std::memory_order_relaxed);
    }
  }

  struct Snapshot
  {
    uint64_t totalErrors;
    uint64_t criticalErrors;
    uint64_t fatalErrors;
    uint64_t lastErrorTimestamp;
    ErrorCode lastErrorCode;
  };

  Snapshot getSnapshot() const noexcept
  {
    return {
        totalErrors.load(std::memory_order_relaxed),
        criticalErrors.load(std::memory_order_relaxed),
        fatalErrors.load(std::memory_order_relaxed),
        lastErrorTimestamp.load(std::memory_order_relaxed),
        static_cast<ErrorCode>(lastErrorCode.load(std::memory_order_relaxed))};
  }
};

// Global error statistics (thread-safe)
extern thread_local ErrorStats g_threadLocalErrorStats;
extern ErrorStats g_globalErrorStats;

// Fast error reporting for hot paths
inline void reportErrorFast(ErrorCode code, ErrorSeverity severity = ErrorSeverity::ERROR) noexcept
{
  g_threadLocalErrorStats.recordError(code, severity);
  g_globalErrorStats.recordError(code, severity);
}

// Error policy definitions
namespace error_policy
{
// Use Result<T> for recoverable errors
template <typename T>
using Recoverable = Result<T>;

// Use FastResult<T> for hot path operations
template <typename T>
using HotPath = FastResult<T>;

// Programming error - should terminate in debug builds
[[noreturn]] inline void invariant_violated(std::string_view msg,
                                            std::source_location location = std::source_location::current())
{
#ifdef NDEBUG
  // In release builds, throw an exception
  throw std::logic_error(std::format("Invariant violated at {}:{}: {}",
                                     location.file_name(), location.line(), msg));
#else
  // In debug builds, abort immediately
  std::cerr << std::format("FATAL: Invariant violated at {}:{}: {}\n",
                           location.file_name(), location.line(), msg);
  std::abort();
#endif
}

// Contract violation - precondition/postcondition failed
[[noreturn]] inline void contract_violated(std::string_view contract_type, std::string_view msg,
                                           std::source_location location = std::source_location::current())
{
#ifdef NDEBUG
  throw std::logic_error(std::format("{} violation at {}:{}: {}",
                                     contract_type, location.file_name(), location.line(), msg));
#else
  std::cerr << std::format("FATAL: {} violation at {}:{}: {}\n",
                           contract_type, location.file_name(), location.line(), msg);
  std::abort();
#endif
}
}  // namespace error_policy

// Async result type for future error handling
template <typename T>
using AsyncResult = std::future<Result<T>>;

// Enhanced macro for debug assertions
#define FLOX_ASSERT(condition, message)                \
  do                                                   \
  {                                                    \
    if (!(condition))                                  \
    {                                                  \
      flox::error_policy::invariant_violated(message); \
    }                                                  \
  } while (false)

// Contract macros
#define FLOX_PRECONDITION(condition, message)                         \
  do                                                                  \
  {                                                                   \
    if (!(condition))                                                 \
    {                                                                 \
      flox::error_policy::contract_violated("Precondition", message); \
    }                                                                 \
  } while (false)

#define FLOX_POSTCONDITION(condition, message)                         \
  do                                                                   \
  {                                                                    \
    if (!(condition))                                                  \
    {                                                                  \
      flox::error_policy::contract_violated("Postcondition", message); \
    }                                                                  \
  } while (false)

}  // namespace flox
