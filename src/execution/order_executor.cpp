/*
 * Flox Engine - Order Executor Implementation
 * Developed by Evgenii Makarov (https://github.com/eeiaao)
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/execution/order_executor.h"

#include <format>
#include <future>
#include <mutex>
#include <random>

namespace flox
{

OrderExecutor::OrderExecutor(const Config& config)
    : _config(config)
{
  _healthStatus.lastActivity = std::chrono::steady_clock::now();
  _healthStatus.lastSuccessfulSubmission = std::chrono::steady_clock::now();
}

Result<OrderId> OrderExecutor::submitOrder(const Order& order)
{
  auto startTime = std::chrono::high_resolution_clock::now();

  // Validate order if enabled
  if (_config.enableOrderValidation)
  {
    if (auto validation = validateOrder(order); !validation)
    {
      std::unique_lock lock(_statsMutex);
      _stats.failedSubmissions++;
      _stats.totalSubmissions++;
      _healthStatus.failedSubmissionCount++;
      _healthStatus.lastActivity = std::chrono::steady_clock::now();
      _recentErrors.push_back(validation.error());
      _healthStatus.lastError = validation.error();

      // Keep only recent errors
      if (_recentErrors.size() > 100)
      {
        _recentErrors.erase(_recentErrors.begin(), _recentErrors.begin() + 10);
      }

      return std::unexpected(validation.error());
    }
  }

  // Check pending order limits
  {
    std::shared_lock lock(_orderMutex);
    if (_activeOrders.size() >= _config.maxPendingOrders)
    {
      auto error = FLOX_ERROR(ORDER_EXECUTION_TIMEOUT,
                              std::format("Too many pending orders: {}/{}", _activeOrders.size(), _config.maxPendingOrders));

      // Release order lock before updating stats
      lock.~shared_lock();

      std::unique_lock statsLock(_statsMutex);
      _healthStatus.lastActivity = std::chrono::steady_clock::now();
      _recentErrors.push_back(error);
      _healthStatus.lastError = error;

      // Keep only recent errors
      if (_recentErrors.size() > 100)
      {
        _recentErrors.erase(_recentErrors.begin(), _recentErrors.begin() + 10);
      }

      return std::unexpected(error);
    }
  }

  // Execute with retry mechanism
  auto result = executeWithRetry("submit", [&]() -> Result<OrderId>
                                 { return doSubmitOrder(order); });

  auto endTime = std::chrono::high_resolution_clock::now();
  auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);

  std::unique_lock lock(_statsMutex);
  _stats.totalSubmissions++;
  _healthStatus.lastActivity = std::chrono::steady_clock::now();

  if (result)
  {
    _stats.successfulSubmissions++;
    _healthStatus.lastSuccessfulSubmission = std::chrono::steady_clock::now();
  }
  else
  {
    _stats.failedSubmissions++;
    _healthStatus.failedSubmissionCount++;
    _recentErrors.push_back(result.error());
    _healthStatus.lastError = result.error();

    // Keep only recent errors
    if (_recentErrors.size() > 100)
    {
      _recentErrors.erase(_recentErrors.begin(), _recentErrors.begin() + 10);
    }
  }

  // Record latency tracking within the existing stats lock
  if (_config.enableLatencyTracking)
  {
    auto& stats = _latencyStats["submit"];

    stats.total += latency;
    stats.count++;
    stats.min = std::min(stats.min, latency);
    stats.max = std::max(stats.max, latency);

    // Update health status average latency
    if (stats.count > 0)
    {
      auto avgNanos = stats.total.count() / stats.count;
      _healthStatus.averageLatency = std::chrono::milliseconds(avgNanos / 1'000'000);
    }
  }

  // Track successful orders outside the stats lock to avoid deadlock
  if (result)
  {
    std::unique_lock orderLock(_orderMutex);
    Order orderCopy = order;
    orderCopy.id = *result;
    _activeOrders[*result] = orderCopy;
    _orderSubmissionTimes[*result] = std::chrono::steady_clock::now();
  }

  return result;
}

VoidResult OrderExecutor::cancelOrder(OrderId orderId)
{
  {
    std::shared_lock lock(_orderMutex);
    if (_activeOrders.find(orderId) == _activeOrders.end())
    {
      return std::unexpected(FLOX_ERROR(ORDER_NOT_FOUND,
                                        std::format("Order {} not found or already completed", orderId)));
    }
  }

  auto result = executeWithRetry("cancel", [&]() -> VoidResult
                                 { return doCancelOrder(orderId); });

  if (result)
  {
    // Update stats first, then order tracking to maintain consistent lock ordering
    {
      std::unique_lock statsLock(_statsMutex);
      _stats.cancellations++;
      _healthStatus.lastActivity = std::chrono::steady_clock::now();
    }

    std::unique_lock lock(_orderMutex);
    _activeOrders.erase(orderId);
    _orderSubmissionTimes.erase(orderId);
  }
  else
  {
    std::unique_lock statsLock(_statsMutex);
    _healthStatus.lastActivity = std::chrono::steady_clock::now();
    _recentErrors.push_back(result.error());
    _healthStatus.lastError = result.error();

    // Keep only recent errors
    if (_recentErrors.size() > 100)
    {
      _recentErrors.erase(_recentErrors.begin(), _recentErrors.begin() + 10);
    }
  }

  return result;
}

Result<OrderId> OrderExecutor::modifyOrder(OrderId orderId, Price newPrice, Quantity newQuantity)
{
  {
    std::shared_lock lock(_orderMutex);
    if (_activeOrders.find(orderId) == _activeOrders.end())
    {
      return std::unexpected(FLOX_ERROR(ORDER_NOT_FOUND,
                                        std::format("Order {} not found for modification", orderId)));
    }
  }

  auto result = executeWithRetry("modify", [&]() -> Result<OrderId>
                                 { return doModifyOrder(orderId, newPrice, newQuantity); });

  if (result)
  {
    // Update stats first, then order tracking to maintain consistent lock ordering
    {
      std::unique_lock statsLock(_statsMutex);
      _stats.modifications++;
      _healthStatus.lastActivity = std::chrono::steady_clock::now();
    }

    // Update stored order information
    std::unique_lock lock(_orderMutex);
    if (auto it = _activeOrders.find(orderId); it != _activeOrders.end())
    {
      it->second.price = newPrice;
      it->second.quantity = newQuantity;
      // If the order ID changed, update tracking
      if (*result != orderId)
      {
        _activeOrders[*result] = it->second;
        _activeOrders.erase(it);
        _orderSubmissionTimes[*result] = _orderSubmissionTimes[orderId];
        _orderSubmissionTimes.erase(orderId);
      }
    }
  }
  else
  {
    std::unique_lock statsLock(_statsMutex);
    _healthStatus.lastActivity = std::chrono::steady_clock::now();
    _recentErrors.push_back(result.error());
    _healthStatus.lastError = result.error();

    // Keep only recent errors
    if (_recentErrors.size() > 100)
    {
      _recentErrors.erase(_recentErrors.begin(), _recentErrors.begin() + 10);
    }
  }

  return result;
}

Result<OrderStatus> OrderExecutor::getOrderStatus(OrderId orderId) const
{
  {
    std::shared_lock lock(_orderMutex);
    if (_activeOrders.find(orderId) == _activeOrders.end())
    {
      return std::unexpected(FLOX_ERROR(ORDER_NOT_FOUND,
                                        std::format("Order {} not found", orderId)));
    }
  }

  return doGetOrderStatus(orderId);
}

Result<std::vector<Order>> OrderExecutor::getActiveOrders() const
{
  std::shared_lock lock(_orderMutex);
  std::vector<Order> orders;
  orders.reserve(_activeOrders.size());

  for (const auto& [id, order] : _activeOrders)
  {
    orders.push_back(order);
  }

  return orders;
}

VoidResult OrderExecutor::validateOrder(const Order& order) const
{
  // Validate quantity
  if (auto result = validateOrderQuantity(order); !result)
  {
    return result;
  }

  // Validate price
  if (auto result = validateOrderPrice(order); !result)
  {
    return result;
  }

  // Validate symbol
  if (auto result = validateOrderSymbol(order); !result)
  {
    return result;
  }

  // Validate limits
  if (auto result = validateOrderLimits(order); !result)
  {
    return result;
  }

  return {};
}

Result<ExecutorHealthStatus> OrderExecutor::getHealthStatus() const
{
  std::shared_lock lock(_statsMutex);
  return _healthStatus;
}

OrderExecutor::ExecutionStatistics OrderExecutor::getStatistics() const
{
  std::shared_lock lock(_statsMutex);
  return _stats;
}

void OrderExecutor::resetStatistics()
{
  std::unique_lock lock(_statsMutex);
  _stats = ExecutionStatistics{};
  _stats.startTime = std::chrono::steady_clock::now();
}

void OrderExecutor::reportError(const FloxError& error)
{
  std::unique_lock lock(_statsMutex);
  _recentErrors.push_back(error);
  _healthStatus.lastError = error;

  // Keep only recent errors
  if (_recentErrors.size() > 100)
  {
    _recentErrors.erase(_recentErrors.begin(), _recentErrors.begin() + 10);
  }
}

void OrderExecutor::updateLastActivity()
{
  std::unique_lock lock(_statsMutex);
  _healthStatus.lastActivity = std::chrono::steady_clock::now();
}

VoidResult OrderExecutor::validateOrderQuantity(const Order& order) const
{
  if (order.quantity.toDouble() <= 0.0)
  {
    return std::unexpected(FLOX_ERROR(ORDER_INVALID_QUANTITY, "Order quantity must be positive"));
  }

  // Additional quantity validation can be added here
  if (order.quantity.toDouble() > 1000000.0)
  {
    return std::unexpected(FLOX_ERROR(ORDER_INVALID_QUANTITY, "Order quantity exceeds maximum limit"));
  }

  return {};
}

VoidResult OrderExecutor::validateOrderPrice(const Order& order) const
{
  if (order.type == OrderType::LIMIT && order.price.toDouble() <= 0.0)
  {
    return std::unexpected(FLOX_ERROR(ORDER_INVALID_PRICE, "Limit order price must be positive"));
  }

  // Additional price validation can be added here
  if (order.price.toDouble() > 1000000.0)
  {
    return std::unexpected(FLOX_ERROR(ORDER_INVALID_PRICE, "Order price exceeds maximum limit"));
  }

  return {};
}

VoidResult OrderExecutor::validateOrderSymbol(const Order& order) const
{
  if (order.symbol == 0)
  {
    return std::unexpected(FLOX_ERROR(VALIDATION_REQUIRED_FIELD_MISSING, "Order symbol is required"));
  }

  return {};
}

VoidResult OrderExecutor::validateOrderLimits(const Order& order) const
{
  // Check order value limits
  double orderValue = order.price.toDouble() * order.quantity.toDouble();
  if (orderValue > 10000000.0)
  {  // $10M limit
    return std::unexpected(FLOX_ERROR(VALIDATION_OUT_OF_RANGE,
                                      std::format("Order value ${:.2f} exceeds limit", orderValue)));
  }

  return {};
}

template <typename F>
auto OrderExecutor::executeWithRetry(const std::string& operation, F&& func) -> std::invoke_result_t<F>
{
  using ReturnType = std::invoke_result_t<F>;

  for (size_t attempt = 0; attempt <= _config.maxRetryAttempts; ++attempt)
  {
    if (attempt > 0)
    {
      std::this_thread::sleep_for(_config.retryDelay * attempt);  // Exponential backoff
    }

    auto result = func();

    if (result || attempt == _config.maxRetryAttempts)
    {
      return result;
    }

    // Note: Don't log retry attempts here to avoid recursive locking
    // The final error will be logged by the calling method
  }

  // This should never be reached due to the logic above, but included for completeness
  if constexpr (std::is_same_v<ReturnType, VoidResult>)
  {
    return std::unexpected(FLOX_ERROR(ORDER_EXECUTION_TIMEOUT, "Maximum retry attempts exceeded"));
  }
  else
  {
    return std::unexpected(FLOX_ERROR(ORDER_EXECUTION_TIMEOUT, "Maximum retry attempts exceeded"));
  }
}

void OrderExecutor::recordLatency(const std::string& operation, std::chrono::nanoseconds latency)
{
  std::unique_lock lock(_statsMutex);
  auto& stats = _latencyStats[operation];

  stats.total += latency;
  stats.count++;
  stats.min = std::min(stats.min, latency);
  stats.max = std::max(stats.max, latency);

  // Update health status average latency
  if (operation == "submit" && stats.count > 0)
  {
    auto avgNanos = stats.total.count() / stats.count;
    _healthStatus.averageLatency = std::chrono::milliseconds(avgNanos / 1'000'000);
  }
}

// MockOrderExecutor implementation

Result<OrderId> MockOrderExecutor::doSubmitOrder(const Order& order)
{
  if (auto result = simulateFailure<OrderId>("submit", [&]() -> OrderId
                                             { return _nextOrderId.fetch_add(1); });
      !result)
  {
    return std::unexpected(result.error());
  }
  else
  {
    return *result;
  }
}

VoidResult MockOrderExecutor::doCancelOrder(OrderId orderId)
{
  return simulateFailure<void>("cancel", [&]()
                               {
                                 // Mock cancellation always succeeds if not failed by simulation
                               });
}

Result<OrderId> MockOrderExecutor::doModifyOrder(OrderId orderId, Price newPrice, Quantity newQuantity)
{
  if (auto result = simulateFailure<OrderId>("modify", [&]() -> OrderId
                                             {
                                               return orderId;  // Same order ID for modification
                                             });
      !result)
  {
    return std::unexpected(result.error());
  }
  else
  {
    return *result;
  }
}

Result<OrderStatus> MockOrderExecutor::doGetOrderStatus(OrderId orderId) const
{
  // Mock status - randomly return different statuses
  std::uniform_int_distribution<int> dist(0, 3);
  int status = dist(_gen);

  switch (status)
  {
    case 0:
      return OrderStatus::NEW;
    case 1:
      return OrderStatus::PARTIALLY_FILLED;
    case 2:
      return OrderStatus::FILLED;
    case 3:
      return OrderStatus::CANCELED;
    default:
      return OrderStatus::REJECTED;
  }
}

template <typename T>
Result<T> MockOrderExecutor::simulateFailure(const std::string& operation, std::function<T()> successFunc) const
{
  auto it = _failureModes.find(operation);
  if (it != _failureModes.end())
  {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    if (dist(_gen) < it->second.failureRate)
    {
      return std::unexpected(it->second.error);
    }
  }

  if constexpr (std::is_void_v<T>)
  {
    successFunc();
    return {};
  }
  else
  {
    return successFunc();
  }
}

}  // namespace flox
