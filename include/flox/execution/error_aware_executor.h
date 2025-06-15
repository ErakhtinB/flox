/*
 * Flox Engine - Error-Aware Order Executor
 * Developed by Evgenii Makarov (https://github.com/eeiaao)
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/common.h"
#include "flox/execution/abstract_executor.h"
#include "flox/execution/order.h"
#include "flox/util/error/error_system.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <random>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace flox
{

// OrderStatus is already defined in order.h

// Executor health status
struct ExecutorHealthStatus
{
  bool isHealthy = true;
  std::chrono::steady_clock::time_point lastSuccessfulSubmission;
  std::chrono::steady_clock::time_point lastActivity;
  size_t pendingOrderCount = 0;
  size_t failedSubmissionCount = 0;
  std::optional<FloxError> lastError;

  // Connection status if applicable
  bool isConnected = true;
  std::chrono::milliseconds averageLatency{0};
};

// Error-aware order execution interface
class IErrorAwareOrderExecutor
{
 public:
  virtual ~IErrorAwareOrderExecutor() = default;

  // Submit order with comprehensive error reporting
  virtual Result<OrderId> submitOrder(const Order& order) = 0;

  // Cancel order with error handling
  virtual VoidResult cancelOrder(OrderId orderId) = 0;

  // Modify existing order
  virtual Result<OrderId> modifyOrder(OrderId orderId, Price newPrice, Quantity newQuantity) = 0;

  // Query order status
  virtual Result<OrderStatus> getOrderStatus(OrderId orderId) const = 0;

  // Get all active orders
  virtual Result<std::vector<Order>> getActiveOrders() const = 0;

  // Validate order before submission
  virtual VoidResult validateOrder(const Order& order) const = 0;

  // Check executor health
  virtual Result<ExecutorHealthStatus> getHealthStatus() const = 0;
};

// Enhanced order executor implementation
class ErrorAwareOrderExecutor : public IErrorAwareOrderExecutor
{
 public:
  struct Config
  {
    std::chrono::milliseconds submitTimeout{5000};
    std::chrono::milliseconds cancelTimeout{3000};
    size_t maxPendingOrders = 1000;
    size_t maxRetryAttempts = 3;
    std::chrono::milliseconds retryDelay{100};
    bool enableOrderValidation = true;
    bool enableLatencyTracking = true;

    // Default constructor
    Config() = default;
  };

  explicit ErrorAwareOrderExecutor(const Config& config);
  ErrorAwareOrderExecutor() : ErrorAwareOrderExecutor(Config{}) {}
  ~ErrorAwareOrderExecutor() override = default;

  // IErrorAwareOrderExecutor implementation
  Result<OrderId> submitOrder(const Order& order) override;
  VoidResult cancelOrder(OrderId orderId) override;
  Result<OrderId> modifyOrder(OrderId orderId, Price newPrice, Quantity newQuantity) override;
  Result<OrderStatus> getOrderStatus(OrderId orderId) const override;
  Result<std::vector<Order>> getActiveOrders() const override;
  VoidResult validateOrder(const Order& order) const override;
  Result<ExecutorHealthStatus> getHealthStatus() const override;

  // Configuration and control
  void setConfig(const Config& config) { _config = config; }
  const Config& getConfig() const { return _config; }

  // Statistics and monitoring
  struct ExecutionStatistics
  {
    size_t totalSubmissions = 0;
    size_t successfulSubmissions = 0;
    size_t failedSubmissions = 0;
    size_t cancellations = 0;
    size_t modifications = 0;
    std::chrono::nanoseconds averageSubmissionLatency{0};
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
  };

  ExecutionStatistics getStatistics() const;
  void resetStatistics();

 protected:
  // Template method pattern for different executor implementations
  virtual Result<OrderId> doSubmitOrder(const Order& order) = 0;
  virtual VoidResult doCancelOrder(OrderId orderId) = 0;
  virtual Result<OrderId> doModifyOrder(OrderId orderId, Price newPrice, Quantity newQuantity) = 0;
  virtual Result<OrderStatus> doGetOrderStatus(OrderId orderId) const = 0;

  // Error reporting
  void reportError(const FloxError& error);
  void updateLastActivity();

 private:
  // Order validation helpers
  VoidResult validateOrderQuantity(const Order& order) const;
  VoidResult validateOrderPrice(const Order& order) const;
  VoidResult validateOrderSymbol(const Order& order) const;
  VoidResult validateOrderLimits(const Order& order) const;

  // Retry mechanism with exponential backoff
  template <typename F>
  auto executeWithRetry(const std::string& operation, F&& func) -> std::invoke_result_t<F>;

  // Latency tracking
  void recordLatency(const std::string& operation, std::chrono::nanoseconds latency);

  Config _config;

  // Order tracking
  mutable std::shared_mutex _orderMutex;
  std::unordered_map<OrderId, Order> _activeOrders;
  std::unordered_map<OrderId, std::chrono::steady_clock::time_point> _orderSubmissionTimes;

  // Health and statistics
  mutable std::shared_mutex _statsMutex;
  ExecutionStatistics _stats;
  ExecutorHealthStatus _healthStatus;
  std::vector<FloxError> _recentErrors;

  // Latency tracking
  struct LatencyStats
  {
    std::chrono::nanoseconds total{0};
    size_t count = 0;
    std::chrono::nanoseconds min{std::chrono::nanoseconds::max()};
    std::chrono::nanoseconds max{0};
  };

  std::unordered_map<std::string, LatencyStats> _latencyStats;

 protected:
  // Order ID generation
  std::atomic<OrderId> _nextOrderId{1};
};

// Concrete implementation for demo/testing
class MockErrorAwareOrderExecutor : public ErrorAwareOrderExecutor
{
 public:
  explicit MockErrorAwareOrderExecutor(const Config& config)
      : ErrorAwareOrderExecutor(config)
  {
  }

  MockErrorAwareOrderExecutor() : MockErrorAwareOrderExecutor(Config{}) {}

  // Simulate different failure scenarios for testing
  void setFailureMode(const std::string& operation, double failureRate, FloxError error)
  {
    _failureModes[operation] = {failureRate, error};
  }

  void clearFailureMode(const std::string& operation)
  {
    _failureModes.erase(operation);
  }

 protected:
  Result<OrderId> doSubmitOrder(const Order& order) override;
  VoidResult doCancelOrder(OrderId orderId) override;
  Result<OrderId> doModifyOrder(OrderId orderId, Price newPrice, Quantity newQuantity) override;
  Result<OrderStatus> doGetOrderStatus(OrderId orderId) const override;

 private:
  struct FailureMode
  {
    double failureRate = 0.0;
    FloxError error = FLOX_ERROR(ORDER_REJECTED_BY_EXCHANGE, "Default error");
  };

  std::unordered_map<std::string, FailureMode> _failureModes;
  mutable std::random_device _rd;
  mutable std::mt19937 _gen{_rd()};

  // Simulate random failures based on configured rates
  template <typename T>
  Result<T> simulateFailure(const std::string& operation, std::function<T()> successFunc) const;
};

// RAII helper for order lifecycle management
class OrderLifecycleGuard
{
 public:
  OrderLifecycleGuard(IErrorAwareOrderExecutor& executor, const Order& order)
      : _executor(executor), _order(order)
  {
    auto result = _executor.submitOrder(_order);
    if (result)
    {
      _orderId = *result;
      _submitted = true;
    }
    else
    {
      throw std::runtime_error("Failed to submit order: " + result.error().toString());
    }
  }

  ~OrderLifecycleGuard()
  {
    if (_submitted && _orderId)
    {
      // Try to cancel if still active (don't throw in destructor)
      _executor.cancelOrder(*_orderId);
    }
  }

  // Non-copyable, moveable
  OrderLifecycleGuard(const OrderLifecycleGuard&) = delete;
  OrderLifecycleGuard& operator=(const OrderLifecycleGuard&) = delete;

  OrderLifecycleGuard(OrderLifecycleGuard&&) = default;
  OrderLifecycleGuard& operator=(OrderLifecycleGuard&&) = default;

  std::optional<OrderId> getOrderId() const { return _orderId; }
  bool isSubmitted() const { return _submitted; }

  Result<OrderStatus> getStatus() const
  {
    if (!_orderId)
    {
      return std::unexpected(FLOX_ERROR(ORDER_NOT_FOUND, "Order was not successfully submitted"));
    }
    return _executor.getOrderStatus(*_orderId);
  }

 private:
  IErrorAwareOrderExecutor& _executor;
  Order _order;
  std::optional<OrderId> _orderId;
  bool _submitted = false;
};

// Utility functions for error-aware order handling
namespace order_utils
{

// Chain multiple order operations with error propagation
template <typename... Fs>
VoidResult chainOrderOperations(Fs&&... operations)
{
  VoidResult result;

  ((result = operations(), result) && ...);

  return result;
}

// Execute order with timeout and error handling
template <typename F>
auto executeWithTimeout(std::chrono::milliseconds timeout, F&& operation)
    -> std::invoke_result_t<F>
{
  using ReturnType = std::invoke_result_t<F>;

  auto future = std::async(std::launch::async, std::forward<F>(operation));

  if (future.wait_for(timeout) == std::future_status::timeout)
  {
    if constexpr (std::is_same_v<ReturnType, VoidResult>)
    {
      return std::unexpected(FLOX_ERROR(ORDER_EXECUTION_TIMEOUT, "Operation timed out"));
    }
    else
    {
      return std::unexpected(FLOX_ERROR(ORDER_EXECUTION_TIMEOUT, "Operation timed out"));
    }
  }

  return future.get();
}

// Batch order operations with individual error handling
template <typename Container, typename F>
std::vector<std::pair<typename Container::value_type, Result<std::invoke_result_t<F, typename Container::value_type>>>>
batchExecute(const Container& items, F&& operation)
{
  std::vector<std::pair<typename Container::value_type, Result<std::invoke_result_t<F, typename Container::value_type>>>> results;
  results.reserve(items.size());

  for (const auto& item : items)
  {
    try
    {
      results.emplace_back(item, operation(item));
    }
    catch (const std::exception& e)
    {
      using ResultType = Result<std::invoke_result_t<F, typename Container::value_type>>;
      results.emplace_back(item,
                           ResultType{std::unexpected(FLOX_ERROR(ORDER_EXECUTION_TIMEOUT,
                                                                 std::format("Batch operation failed: {}", e.what())))});
    }
  }

  return results;
}

}  // namespace order_utils

}  // namespace flox
