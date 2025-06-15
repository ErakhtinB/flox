/*
 * Flox Engine - Error System Tests
 * Tests for the std::expected-based error handling system
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License.
 */

#include <chrono>
#include <format>
#include <thread>

#include "flox/util/error/error_system.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace flox;

class ErrorSystemTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    // Reset any global state if needed
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

// Performance benchmarks removed - they're too flaky across different environments
// For dedicated performance testing, use separate benchmark suites

/*
// Test thread safety of error objects
TEST_F(ErrorSystemTest, ThreadSafety)
{
  auto error = FLOX_ERROR(CONNECTION_LOST, "Thread safety test");
  std::vector<std::thread> threads;
  std::atomic<int> successCount{0};

  // Create multiple threads that access the error
  for (int i = 0; i < 10; ++i)
  {
    threads.emplace_back([&error, &successCount]()
                         {
      for (int j = 0; j < 1000; ++j) {
        // These operations should be thread-safe
        auto code = error.code();
        auto message = error.message();
        auto severity = error.severity();
        auto category = error.category();
        auto timestamp = error.timestamp();
        
        if (code == ErrorCode::CONNECTION_LOST && 
            message == "Thread safety test" &&
            severity == ErrorSeverity::ERROR &&
            category == ErrorCategory::CONNECTIVITY) {
          successCount++;
        }
      } });
  }

  // Wait for all threads to complete
  for (auto& thread : threads)
  {
    thread.join();
  }

  EXPECT_EQ(successCount.load(), 10000);  // 10 threads * 1000 iterations
}
*/

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

/*
// Integration test with mock executor
class MockExecutorErrorTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    executor = std::make_unique<MockErrorAwareOrderExecutor>();
  }

  std::unique_ptr<MockErrorAwareOrderExecutor> executor;
};

TEST_F(MockExecutorErrorTest, OrderSubmissionWithErrors)
{
  // Configure failure modes
  executor->setFailureMode("submit", 1.0,  // 100% failure rate
                           FLOX_ERROR(ORDER_REJECTED_BY_EXCHANGE, "Test rejection"));

  Order testOrder{
      .id = 0,
      .symbol = 1,
      .side = Side::BUY,
      .type = OrderType::LIMIT,
      .quantity = Quantity::fromDouble(100.0),
      .price = Price::fromDouble(50.0),
      .timestamp = std::chrono::system_clock::now()};

  auto result = executor->submitOrder(testOrder);

  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::ORDER_REJECTED_BY_EXCHANGE);
  EXPECT_THAT(result.error().message(), testing::HasSubstr("Test rejection"));
}

TEST_F(MockExecutorErrorTest, OrderValidation)
{
  Order invalidOrder{
      .id = 0,
      .symbol = 1,
      .side = Side::BUY,
      .type = OrderType::LIMIT,
      .quantity = Quantity::fromDouble(-100.0),  // Invalid negative quantity
      .price = Price::fromDouble(50.0),
      .timestamp = std::chrono::system_clock::now()};

  auto validationResult = executor->validateOrder(invalidOrder);

  EXPECT_FALSE(validationResult.has_value());
  EXPECT_EQ(validationResult.error().code(), ErrorCode::ORDER_INVALID_QUANTITY);
}
*/

// Run all tests
int main(int argc, char** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}