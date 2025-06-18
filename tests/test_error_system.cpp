/*
 * Flox Engine - Error System Tests
 * Tests for the std::expected-based error handling system
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License.
 */

#include <atomic>
#include <chrono>
#include <format>
#include <thread>
#include <vector>

#include "flox/engine/engine_config.h"
#include "flox/position/position_manager.h"
#include "flox/util/error/error_system.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace flox;

class ErrorSystemTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    // Reset error statistics
    g_globalErrorStats.reset();
  }

  void TearDown() override
  {
    // Clean up after tests
  }
};

// Test basic error creation and properties
TEST_F(ErrorSystemTest, ErrorCreationAndProperties)
{
  auto error = FLOX_ERROR(ORDER_INVALID_QUANTITY, "Test error message");

  EXPECT_EQ(error.code(), ErrorCode::ORDER_INVALID_QUANTITY);
  EXPECT_EQ(error.message(), "Test error message");
  EXPECT_EQ(error.severity(), ErrorSeverity::ERROR);
  EXPECT_EQ(error.category(), ErrorCategory::ORDER_EXECUTION);

  // Test timestamp is recent
  auto now = std::chrono::steady_clock::now();
  auto errorTime = error.timestamp();
  auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - errorTime);
  EXPECT_LT(diff.count(), 100);  // Should be within 100ms
}

// Test different severity levels
TEST_F(ErrorSystemTest, ErrorSeverityLevels)
{
  auto infoError = FloxError::Info(ErrorCode::ENGINE_NOT_STARTED, "Info message");
  auto warningError = FloxError::Warning(ErrorCode::MARKET_DATA_STALE_DATA, "Warning message");
  auto criticalError = FloxError::Critical(ErrorCode::CONNECTION_LOST, "Critical message");
  auto fatalError = FloxError::Fatal(ErrorCode::MEMORY_ALLOCATION_FAILED, "Fatal message");

  EXPECT_EQ(infoError.severity(), ErrorSeverity::INFO);
  EXPECT_EQ(warningError.severity(), ErrorSeverity::WARNING);
  EXPECT_EQ(criticalError.severity(), ErrorSeverity::CRITICAL);
  EXPECT_EQ(fatalError.severity(), ErrorSeverity::FATAL);
}

// Test error context chaining
TEST_F(ErrorSystemTest, ErrorContextChaining)
{
  auto baseError = FLOX_ERROR(ORDER_REJECTED_BY_EXCHANGE, "Order rejected");
  auto contextError = baseError.withContext("While processing strategy signal");

  EXPECT_EQ(contextError.code(), baseError.code());
  EXPECT_EQ(contextError.severity(), baseError.severity());
  EXPECT_THAT(contextError.message(), testing::HasSubstr("While processing strategy signal"));
  EXPECT_THAT(contextError.message(), testing::HasSubstr("Order rejected"));
}

// Test Result<T> success cases
TEST_F(ErrorSystemTest, ResultSuccessCases)
{
  Result<int> successResult = 42;

  EXPECT_TRUE(isSuccess(successResult));
  EXPECT_FALSE(isError(successResult));
  EXPECT_TRUE(successResult.has_value());
  EXPECT_EQ(successResult.value(), 42);
  EXPECT_EQ(*successResult, 42);
}

// Test Result<T> error cases
TEST_F(ErrorSystemTest, ResultErrorCases)
{
  Result<int> errorResult = std::unexpected(FLOX_ERROR(VALIDATION_OUT_OF_RANGE, "Invalid value"));

  EXPECT_FALSE(isSuccess(errorResult));
  EXPECT_TRUE(isError(errorResult));
  EXPECT_FALSE(errorResult.has_value());
  EXPECT_EQ(errorResult.error().code(), ErrorCode::VALIDATION_OUT_OF_RANGE);
}

// Test andThen error propagation
TEST_F(ErrorSystemTest, AndThenErrorPropagation)
{
  // Success case
  auto successChain = andThen(Result<int>{10}, [](int value) -> Result<int>
                              { return value * 2; });

  EXPECT_TRUE(successChain.has_value());
  EXPECT_EQ(*successChain, 20);

  // Error case - error should propagate
  Result<int> errorResult = std::unexpected(FLOX_ERROR(VALIDATION_OUT_OF_RANGE, "Initial error"));
  auto errorChain = andThen(std::move(errorResult), [](int value) -> Result<int>
                            {
                              return value * 2;  // This should not execute
                            });

  EXPECT_FALSE(errorChain.has_value());
  EXPECT_EQ(errorChain.error().code(), ErrorCode::VALIDATION_OUT_OF_RANGE);

  // Error in chain
  auto chainError = andThen(Result<int>{10}, [](int value) -> Result<int>
                            {
    if (value > 5) {
      return std::unexpected(FLOX_ERROR(VALIDATION_OUT_OF_RANGE, "Value too large"));
    }
    return value * 2; });

  EXPECT_FALSE(chainError.has_value());
  EXPECT_EQ(chainError.error().code(), ErrorCode::VALIDATION_OUT_OF_RANGE);
}

// Test orElse error recovery
TEST_F(ErrorSystemTest, OrElseErrorRecovery)
{
  // Success case - recovery function should not be called
  auto successResult = orElse(Result<int>{42}, [](const FloxError&) -> Result<int>
                              {
                                return 999;  // Should not be reached
                              });

  EXPECT_TRUE(successResult.has_value());
  EXPECT_EQ(*successResult, 42);

  // Error case - recovery function should be called
  Result<int> errorResult = std::unexpected(FLOX_ERROR(CONNECTION_LOST, "Connection failed"));
  auto recoveredResult = orElse(std::move(errorResult), [](const FloxError& err) -> Result<int>
                                {
                                  EXPECT_EQ(err.code(), ErrorCode::CONNECTION_LOST);
                                  return 999;  // Recovery value
                                });

  EXPECT_TRUE(recoveredResult.has_value());
  EXPECT_EQ(*recoveredResult, 999);
}

// Test mapError function
TEST_F(ErrorSystemTest, MapErrorTransformation)
{
  Result<int> originalResult = std::unexpected(FLOX_ERROR(CONNECTION_LOST, "Original error"));

  auto mappedResult = mapError(std::move(originalResult), [](FloxError err) -> FloxError
                               { return err.withContext("Mapped context"); });

  EXPECT_FALSE(mappedResult.has_value());
  EXPECT_THAT(mappedResult.error().message(), testing::HasSubstr("Mapped context"));
  EXPECT_THAT(mappedResult.error().message(), testing::HasSubstr("Original error"));
}

// Test VoidResult
TEST_F(ErrorSystemTest, VoidResult)
{
  // Success case
  VoidResult successResult;
  EXPECT_TRUE(successResult.has_value());
  EXPECT_TRUE(isSuccess(successResult));

  // Error case
  VoidResult errorResult = std::unexpected(FLOX_ERROR(ENGINE_NOT_STARTED, "Engine not started"));
  EXPECT_FALSE(errorResult.has_value());
  EXPECT_TRUE(isError(errorResult));
  EXPECT_EQ(errorResult.error().code(), ErrorCode::ENGINE_NOT_STARTED);
}

// Test error category mapping
TEST_F(ErrorSystemTest, ErrorCategoryMapping)
{
  struct TestCase
  {
    ErrorCode code;
    ErrorCategory expectedCategory;
  };

  std::vector<TestCase> testCases = {
      {ErrorCode::ENGINE_NOT_STARTED, ErrorCategory::ENGINE},
      {ErrorCode::MARKET_DATA_QUEUE_FULL, ErrorCategory::MARKET_DATA},
      {ErrorCode::ORDER_INVALID_QUANTITY, ErrorCategory::ORDER_EXECUTION},
      {ErrorCode::RISK_POSITION_LIMIT_EXCEEDED, ErrorCategory::RISK_MANAGEMENT},
      {ErrorCode::CONNECTION_LOST, ErrorCategory::CONNECTIVITY},
      {ErrorCode::VALIDATION_OUT_OF_RANGE, ErrorCategory::VALIDATION},
      {ErrorCode::POSITION_INSUFFICIENT_BALANCE, ErrorCategory::POSITION},
  };

  for (const auto& testCase : testCases)
  {
    auto error = FloxError(testCase.code, "Test message");
    EXPECT_EQ(error.category(), testCase.expectedCategory)
        << "Failed for error code: " << static_cast<int>(testCase.code);
  }
}

// Test macro helper functions
TEST_F(ErrorSystemTest, MacroHelpers)
{
  auto regularError = FLOX_ERROR(ORDER_INVALID_QUANTITY, "Regular error");
  auto infoError = FLOX_ERROR_INFO(ENGINE_NOT_STARTED, "Info error");
  auto warningError = FLOX_ERROR_WARNING(MARKET_DATA_STALE_DATA, "Warning error");
  auto criticalError = FLOX_ERROR_CRITICAL(CONNECTION_LOST, "Critical error");
  auto fatalError = FLOX_ERROR_FATAL(MEMORY_ALLOCATION_FAILED, "Fatal error");

  EXPECT_EQ(regularError.severity(), ErrorSeverity::ERROR);
  EXPECT_EQ(infoError.severity(), ErrorSeverity::INFO);
  EXPECT_EQ(warningError.severity(), ErrorSeverity::WARNING);
  EXPECT_EQ(criticalError.severity(), ErrorSeverity::CRITICAL);
  EXPECT_EQ(fatalError.severity(), ErrorSeverity::FATAL);
}

// Test simple error chaining scenarios
TEST_F(ErrorSystemTest, SimpleErrorChaining)
{
  // Test simple success chain without nested Results
  auto processValue = [](int input) -> Result<std::string>
  {
    if (input < 0)
    {
      return std::unexpected(FLOX_ERROR(VALIDATION_OUT_OF_RANGE, "Negative input"));
    }

    int doubled = input * 2;
    if (doubled > 100)
    {
      return std::unexpected(FLOX_ERROR(VALIDATION_OUT_OF_RANGE, "Value too large"));
    }

    int final = doubled + 10;
    return std::format("Result: {}", final);
  };

  // Test success case
  auto successResult = processValue(20);
  EXPECT_TRUE(successResult.has_value());
  EXPECT_EQ(*successResult, "Result: 50");  // (20 * 2) + 10 = 50

  // Test error case
  auto errorResult = processValue(-5);
  EXPECT_FALSE(errorResult.has_value());
  EXPECT_EQ(errorResult.error().code(), ErrorCode::VALIDATION_OUT_OF_RANGE);
  EXPECT_THAT(errorResult.error().message(), testing::HasSubstr("Negative input"));

  // Test individual andThen operations
  Result<int> input = 10;
  auto doubled = andThen(std::move(input), [](int value) -> Result<int>
                         { return value * 2; });

  EXPECT_TRUE(doubled.has_value());
  EXPECT_EQ(*doubled, 20);
}

// NEW ENHANCED ERROR SYSTEM TESTS

// Test FastResult for hot paths
TEST_F(ErrorSystemTest, FastResultHotPath)
{
  // Success case
  FastResult<int> successResult = 42;
  EXPECT_TRUE(successResult.has_value());
  EXPECT_EQ(*successResult, 42);

  // Error case
  FastResult<int> errorResult = std::unexpected(ErrorCode::VALIDATION_OUT_OF_RANGE);
  EXPECT_FALSE(errorResult.has_value());
  EXPECT_EQ(errorResult.error(), ErrorCode::VALIDATION_OUT_OF_RANGE);
}

// Test error code expansion
TEST_F(ErrorSystemTest, ErrorCodeExpansion)
{
  auto expandedError = expandError(ErrorCode::ORDER_INVALID_QUANTITY, "Custom message");

  EXPECT_EQ(expandedError.code(), ErrorCode::ORDER_INVALID_QUANTITY);
  EXPECT_EQ(expandedError.message(), "Custom message");
  EXPECT_EQ(expandedError.severity(), ErrorSeverity::ERROR);

  // Test with empty message
  auto autoMessageError = expandError(ErrorCode::CONNECTION_LOST);
  EXPECT_THAT(autoMessageError.message(), testing::HasSubstr("Error code: 400"));
}

// Test ErrorCollector
TEST_F(ErrorSystemTest, ErrorCollector)
{
  ErrorCollector collector;

  // Initially empty
  EXPECT_FALSE(collector.hasErrors());
  EXPECT_EQ(collector.count(), 0);

  // Add single error
  collector.add(FLOX_ERROR(ORDER_INVALID_QUANTITY, "First error"));
  EXPECT_TRUE(collector.hasErrors());
  EXPECT_EQ(collector.count(), 1);

  // Add multiple errors
  collector.add(ErrorCode::CONNECTION_LOST, "Second error");
  collector.add(FLOX_ERROR_CRITICAL(MEMORY_ALLOCATION_FAILED, "Third error"));
  EXPECT_EQ(collector.count(), 3);

  // Test finalization with single error
  ErrorCollector singleErrorCollector;
  singleErrorCollector.add(FLOX_ERROR(ORDER_REJECTED_BY_EXCHANGE, "Single error"));
  auto singleResult = singleErrorCollector.finalize();
  EXPECT_FALSE(singleResult.has_value());
  EXPECT_EQ(singleResult.error().code(), ErrorCode::ORDER_REJECTED_BY_EXCHANGE);

  // Test finalization with multiple errors
  auto multiResult = collector.finalize();
  EXPECT_FALSE(multiResult.has_value());
  EXPECT_THAT(multiResult.error().message(), testing::HasSubstr("Multiple errors occurred"));
  EXPECT_THAT(multiResult.error().message(), testing::HasSubstr("3 total"));
  // Should use the highest severity error's code (CRITICAL)
  EXPECT_EQ(multiResult.error().code(), ErrorCode::MEMORY_ALLOCATION_FAILED);
}

// Test lock-free error statistics
TEST_F(ErrorSystemTest, LockFreeErrorStats)
{
  ErrorStats stats;

  // Initially zero
  auto snapshot = stats.getSnapshot();
  EXPECT_EQ(snapshot.totalErrors, 0);
  EXPECT_EQ(snapshot.criticalErrors, 0);
  EXPECT_EQ(snapshot.fatalErrors, 0);

  // Record some errors
  stats.recordError(ErrorCode::ORDER_INVALID_QUANTITY, ErrorSeverity::ERROR);
  stats.recordError(ErrorCode::CONNECTION_LOST, ErrorSeverity::CRITICAL);
  stats.recordError(ErrorCode::MEMORY_ALLOCATION_FAILED, ErrorSeverity::FATAL);

  snapshot = stats.getSnapshot();
  EXPECT_EQ(snapshot.totalErrors, 3);
  EXPECT_EQ(snapshot.criticalErrors, 2);  // CRITICAL + FATAL
  EXPECT_EQ(snapshot.fatalErrors, 1);
  EXPECT_EQ(snapshot.lastErrorCode, ErrorCode::MEMORY_ALLOCATION_FAILED);
  EXPECT_GT(snapshot.lastErrorTimestamp, 0);
}

// Test fast error reporting
TEST_F(ErrorSystemTest, FastErrorReporting)
{
  // Record some errors using fast reporting
  reportErrorFast(ErrorCode::ORDER_INVALID_QUANTITY);
  reportErrorFast(ErrorCode::CONNECTION_LOST, ErrorSeverity::CRITICAL);

  auto globalSnapshot = g_globalErrorStats.getSnapshot();
  EXPECT_EQ(globalSnapshot.totalErrors, 2);
  EXPECT_EQ(globalSnapshot.criticalErrors, 1);

  // Test thread-local stats
  auto threadSnapshot = g_threadLocalErrorStats.getSnapshot();
  EXPECT_EQ(threadSnapshot.totalErrors, 2);
  EXPECT_EQ(threadSnapshot.criticalErrors, 1);
}

// Test error policy assertions
TEST_F(ErrorSystemTest, ErrorPolicyAssertions)
{
  // Test that assertions work in debug mode
  EXPECT_NO_THROW(FLOX_ASSERT(true, "This should not trigger"));

#ifndef NDEBUG
  // In debug mode, this should abort
  EXPECT_DEATH(FLOX_ASSERT(false, "This should abort"), "FATAL: Invariant violated");
#else
  // In release mode, this should throw
  EXPECT_THROW(FLOX_ASSERT(false, "This should throw"), std::logic_error);
#endif
}

// Test contract violations
TEST_F(ErrorSystemTest, ContractViolations)
{
#ifndef NDEBUG
  EXPECT_DEATH(FLOX_PRECONDITION(false, "Precondition failed"), "FATAL: Precondition violation");
  EXPECT_DEATH(FLOX_POSTCONDITION(false, "Postcondition failed"), "FATAL: Postcondition violation");
#else
  EXPECT_THROW(FLOX_PRECONDITION(false, "Precondition failed"), std::logic_error);
  EXPECT_THROW(FLOX_POSTCONDITION(false, "Postcondition failed"), std::logic_error);
#endif
}

// Test enhanced PositionManager error handling
TEST_F(ErrorSystemTest, PositionManagerErrorHandling)
{
  PositionConfig config;
  config.maxPositionSize = Quantity::fromDouble(1000.0);
  PositionManager pm(1, config);

  // Test start/stop - these return void but internally use error handling
  EXPECT_NO_THROW(pm.start());
  EXPECT_NO_THROW(pm.stop());

  // Test valid position operations - getPosition returns Quantity directly
  auto position = pm.getPosition(0);
  EXPECT_EQ(position, Quantity{0});

  // Test position update using internal error-aware methods
  auto updateResult = pm.updatePosition(0, Quantity::fromDouble(100.0));
  EXPECT_TRUE(updateResult.has_value());

  // Verify position changed
  position = pm.getPosition(0);
  EXPECT_EQ(position, Quantity::fromDouble(100.0));

  // Test position limits with a smaller limit
  PositionConfig limitConfig;
  limitConfig.maxPositionSize = Quantity::fromDouble(50.0);
  limitConfig.enableRiskChecks = true;
  PositionManager limitPm(2, limitConfig);

  auto limitResult = limitPm.updatePosition(0, Quantity::fromDouble(100.0));
  EXPECT_FALSE(limitResult.has_value());
  EXPECT_EQ(limitResult.error().code(), ErrorCode::RISK_POSITION_LIMIT_EXCEEDED);
}

// Test order execution with error handling
TEST_F(ErrorSystemTest, OrderExecutionWithErrors)
{
  PositionManager pm(1);
  pm.start();

  Order testOrder{
      .id = 1,
      .side = Side::BUY,
      .price = Price::fromDouble(50.0),
      .quantity = Quantity::fromDouble(100.0),
      .type = OrderType::MARKET,
      .symbol = 0};

  // Test order filled - these return void but handle errors internally
  EXPECT_NO_THROW(pm.onOrderFilled(testOrder));

  // Verify position updated
  auto position = pm.getPosition(0);
  EXPECT_EQ(position, Quantity::fromDouble(100.0));

  // Test partial fill
  EXPECT_NO_THROW(pm.onOrderPartiallyFilled(testOrder, Quantity::fromDouble(50.0)));

  // Verify position updated again
  position = pm.getPosition(0);
  EXPECT_EQ(position, Quantity::fromDouble(150.0));
}

// Test statistics and monitoring
TEST_F(ErrorSystemTest, PositionManagerStatistics)
{
  PositionManager pm(1);
  pm.start();

  // Get initial statistics
  auto statsResult = pm.getStats();
  EXPECT_TRUE(statsResult.has_value());
  EXPECT_EQ(statsResult->totalTrades, 0);

  // Update position
  pm.updatePosition(0, Quantity::fromDouble(100.0));

  // Check updated statistics
  statsResult = pm.getStats();
  EXPECT_TRUE(statsResult.has_value());
  EXPECT_EQ(statsResult->totalTrades, 1);
  EXPECT_EQ(statsResult->maxPositionReached, Quantity::fromDouble(100.0));
}

// Test concurrent error reporting
TEST_F(ErrorSystemTest, ConcurrentErrorReporting)
{
  // Reset stats to ensure clean test
  g_globalErrorStats.reset();
  g_threadLocalErrorStats.reset();

  const int numThreads = 10;
  const int errorsPerThread = 1000;
  std::vector<std::thread> threads;
  std::atomic<int> completedThreads{0};

  // Start multiple threads reporting errors
  for (int i = 0; i < numThreads; ++i)
  {
    threads.emplace_back([&, i]()
                         {
      for (int j = 0; j < errorsPerThread; ++j)
      {
        reportErrorFast(ErrorCode::ORDER_INVALID_QUANTITY);
        if (j % 10 == 0)
        {
          reportErrorFast(ErrorCode::CONNECTION_LOST, ErrorSeverity::CRITICAL);
        }
      }
      completedThreads.fetch_add(1); });
  }

  // Wait for all threads to complete
  for (auto& thread : threads)
  {
    thread.join();
  }

  EXPECT_EQ(completedThreads.load(), numThreads);

  // Check that all errors were recorded
  auto snapshot = g_globalErrorStats.getSnapshot();
  // Each thread reports errorsPerThread regular errors + errorsPerThread/10 critical errors
  const int expectedCriticalErrors = numThreads * (errorsPerThread / 10);
  const int expectedTotalErrors = numThreads * errorsPerThread + expectedCriticalErrors;

  EXPECT_EQ(snapshot.totalErrors, expectedTotalErrors);
  EXPECT_EQ(snapshot.criticalErrors, expectedCriticalErrors);
}

// Test error aggregation with many errors
TEST_F(ErrorSystemTest, ErrorAggregationLargeSet)
{
  ErrorCollector collector;

  // Add many errors
  for (int i = 0; i < 10; ++i)
  {
    collector.add(ErrorCode::ORDER_INVALID_QUANTITY, std::format("Error {}", i));
  }

  auto result = collector.finalize();
  EXPECT_FALSE(result.has_value());
  EXPECT_THAT(result.error().message(), testing::HasSubstr("Multiple errors occurred (10 total)"));
  EXPECT_THAT(result.error().message(), testing::HasSubstr("Error 0"));
  EXPECT_THAT(result.error().message(), testing::HasSubstr("Error 4"));
  EXPECT_THAT(result.error().message(), testing::HasSubstr("... and 5 more errors"));
}

// Run all tests
int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}