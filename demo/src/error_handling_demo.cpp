/*
 * Flox Engine - Error Handling Demo
 * Demonstrates the std::expected-based error handling system
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License.
 */

#include "flox/common.h"
#include "flox/execution/error_aware_executor.h"
#include "flox/execution/order.h"
#include "flox/util/error/error_system.h"

#include <chrono>
#include <format>
#include <iostream>
#include <random>
#include <thread>

using namespace flox;

// Demo class showing error-aware order operations
class ErrorHandlingDemo
{
 public:
  ErrorHandlingDemo() : _executor(createExecutorConfig()) {}

  void runDemo()
  {
    std::cout << "Flox Error Handling System Demo\n";
    std::cout << "================================\n";

    demonstrateBasicErrorHandling();
    demonstrateOrderOperations();
    demonstrateRetryMechanism();
    demonstrateHealthMonitoring();
    demonstrateStatistics();
  }

 private:
  MockErrorAwareOrderExecutor::Config createExecutorConfig()
  {
    MockErrorAwareOrderExecutor::Config config;
    config.maxRetryAttempts = 3;
    config.retryDelay = std::chrono::milliseconds(100);
    config.maxPendingOrders = 10;
    config.enableOrderValidation = true;
    config.enableLatencyTracking = true;
    return config;
  }

  void demonstrateBasicErrorHandling()
  {
    std::cout << "\n=== Basic Error Handling ===\n";

    // Test with invalid order (zero quantity)
    Order invalidOrder{
        .id = 0,
        .side = Side::BUY,
        .price = Price::fromDouble(100.0),
        .quantity = Quantity::fromDouble(0.0),  // Invalid!
        .type = OrderType::LIMIT,
        .symbol = 1};

    auto result = _executor.submitOrder(invalidOrder);
    if (!result)
    {
      std::cout << "✓ Caught validation error: " << result.error().message() << "\n";
    }

    // Test with valid order
    Order validOrder{
        .id = 0,
        .side = Side::BUY,
        .price = Price::fromDouble(100.0),
        .quantity = Quantity::fromDouble(100.0),
        .type = OrderType::LIMIT,
        .symbol = 1};

    result = _executor.submitOrder(validOrder);
    if (result)
    {
      std::cout << "✓ Valid order submitted with ID: " << *result << "\n";

      // Clean up by canceling
      auto cancelResult = _executor.cancelOrder(*result);
      if (cancelResult)
      {
        std::cout << "✓ Order cancelled successfully\n";
      }
    }
  }

  void demonstrateOrderOperations()
  {
    std::cout << "\n=== Order Operations ===\n";

    // Configure some failures for demonstration
    _executor.setFailureMode("submit", 0.3,
                             FLOX_ERROR(ORDER_REJECTED_BY_EXCHANGE, "Exchange temporarily unavailable"));

    std::vector<OrderId> successfulOrders;

    // Submit multiple orders
    for (int i = 0; i < 5; ++i)
    {
      Order order{
          .id = 0,
          .side = (i % 2 == 0) ? Side::BUY : Side::SELL,
          .price = Price::fromDouble(100.0 + i),
          .quantity = Quantity::fromDouble(100.0 + i * 10),
          .type = OrderType::LIMIT,
          .symbol = static_cast<SymbolId>(1 + i % 3)};

      std::cout << std::format("Submitting order {} ({})\n",
                               i + 1, order.side == Side::BUY ? "BUY" : "SELL");

      auto result = _executor.submitOrder(order);
      if (result)
      {
        std::cout << std::format("  ✓ Success: Order ID {}\n", *result);
        successfulOrders.push_back(*result);
      }
      else
      {
        std::cout << std::format("  ✗ Failed: {}\n", result.error().message());
      }
    }

    // Try to modify some orders
    if (!successfulOrders.empty())
    {
      auto orderId = successfulOrders[0];
      std::cout << std::format("\nModifying order {}\n", orderId);

      auto modifyResult = _executor.modifyOrder(orderId,
                                                Price::fromDouble(101.5), Quantity::fromDouble(150.0));

      if (modifyResult)
      {
        std::cout << "  ✓ Order modified successfully\n";
      }
      else
      {
        std::cout << std::format("  ✗ Modify failed: {}\n", modifyResult.error().message());
      }
    }

    // Get active orders
    auto activeResult = _executor.getActiveOrders();
    if (activeResult)
    {
      std::cout << std::format("\nActive orders count: {}\n", activeResult->size());
    }

    // Clean up remaining orders
    for (auto orderId : successfulOrders)
    {
      _executor.cancelOrder(orderId);
    }
  }

  void demonstrateRetryMechanism()
  {
    std::cout << "\n=== Retry Mechanism ===\n";

    // Set high failure rate to trigger retries
    _executor.setFailureMode("submit", 0.8,
                             FLOX_ERROR(CONNECTION_TIMEOUT, "Network timeout"));

    Order order{
        .id = 0,
        .side = Side::BUY,
        .price = Price::fromDouble(100.0),
        .quantity = Quantity::fromDouble(100.0),
        .type = OrderType::LIMIT,
        .symbol = 1};

    std::cout << "Attempting order with high failure rate (should retry):\n";
    auto start = std::chrono::steady_clock::now();

    auto result = _executor.submitOrder(order);

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    if (result)
    {
      std::cout << std::format("✓ Order succeeded after retries (took {}ms)\n", duration.count());
      _executor.cancelOrder(*result);
    }
    else
    {
      std::cout << std::format("✗ Order failed after all retries (took {}ms): {}\n",
                               duration.count(), result.error().message());
    }

    // Reset failure rate
    _executor.clearFailureMode("submit");
  }

  void demonstrateHealthMonitoring()
  {
    std::cout << "\n=== Health Monitoring ===\n";

    auto healthResult = _executor.getHealthStatus();
    if (healthResult)
    {
      const auto& health = *healthResult;
      std::cout << std::format("Health Status:\n");
      std::cout << std::format("  Average Latency: {}ms\n", health.averageLatency.count());
      std::cout << std::format("  Failed Submissions: {}\n", health.failedSubmissionCount);

      auto now = std::chrono::steady_clock::now();
      auto timeSinceActivity = std::chrono::duration_cast<std::chrono::seconds>(
          now - health.lastActivity);
      std::cout << std::format("  Time Since Last Activity: {}s\n", timeSinceActivity.count());
    }
  }

  void demonstrateStatistics()
  {
    std::cout << "\n=== Execution Statistics ===\n";

    auto stats = _executor.getStatistics();
    std::cout << std::format("Total Submissions: {}\n", stats.totalSubmissions);
    std::cout << std::format("Successful: {}\n", stats.successfulSubmissions);
    std::cout << std::format("Failed: {}\n", stats.failedSubmissions);
    std::cout << std::format("Cancellations: {}\n", stats.cancellations);
    std::cout << std::format("Modifications: {}\n", stats.modifications);

    if (stats.totalSubmissions > 0)
    {
      double successRate = (double)stats.successfulSubmissions / stats.totalSubmissions * 100.0;
      std::cout << std::format("Success Rate: {:.1f}%\n", successRate);
    }
  }

  MockErrorAwareOrderExecutor _executor;
};

int main()
{
  try
  {
    ErrorHandlingDemo demo;
    demo.runDemo();

    std::cout << "\n=== Demo completed successfully ===\n";
    return 0;
  }
  catch (const std::exception& e)
  {
    std::cout << "Demo failed with exception: " << e.what() << "\n";
    return 1;
  }
}