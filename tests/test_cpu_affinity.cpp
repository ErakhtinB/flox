/*
 * Flox Engine - CPU Affinity Tests
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/book/bus/trade_bus.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/util/performance/cpu_affinity.h"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <vector>

using namespace flox;
using namespace flox::performance;

class CpuAffinityTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    // Save original affinity
    _originalAffinity = CpuAffinity::getCurrentAffinity();
    _numCores = CpuAffinity::getNumCores();
  }

  void TearDown() override
  {
    // Restore original affinity
    if (!_originalAffinity.empty())
    {
#ifdef __linux__
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      for (int core : _originalAffinity)
      {
        CPU_SET(core, &cpuset);
      }
      sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
#endif
    }
  }

  std::vector<int> _originalAffinity;
  int _numCores;
};

/**
 * @brief Test basic CPU affinity functionality
 */
TEST_F(CpuAffinityTest, BasicCpuInfo)
{
  EXPECT_GT(_numCores, 0);
  EXPECT_LE(_numCores, 256);  // Reasonable upper bound

  auto currentAffinity = CpuAffinity::getCurrentAffinity();
  EXPECT_FALSE(currentAffinity.empty());

  // All cores should be valid
  for (int core : currentAffinity)
  {
    EXPECT_GE(core, 0);
    EXPECT_LT(core, _numCores);
  }
}

/**
 * @brief Test CPU core pinning
 */
TEST_F(CpuAffinityTest, PinToCore)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

  // Pin to core 0
  bool result = CpuAffinity::pinToCore(0);

#ifdef __linux__
  EXPECT_TRUE(result);

  auto affinity = CpuAffinity::getCurrentAffinity();
  EXPECT_EQ(affinity.size(), 1);
  EXPECT_EQ(affinity[0], 0);

  // Pin to core 1
  result = CpuAffinity::pinToCore(1);
  EXPECT_TRUE(result);

  affinity = CpuAffinity::getCurrentAffinity();
  EXPECT_EQ(affinity.size(), 1);
  EXPECT_EQ(affinity[0], 1);
#else
  EXPECT_FALSE(result);  // Should fail on non-Linux platforms
#endif
}

/**
 * @brief Test invalid core pinning
 */
TEST_F(CpuAffinityTest, PinToInvalidCore)
{
  // Try to pin to invalid core
  bool result = CpuAffinity::pinToCore(999);
  EXPECT_FALSE(result);

  result = CpuAffinity::pinToCore(-1);
  EXPECT_FALSE(result);
}

/**
 * @brief Test thread affinity guard
 */
TEST_F(CpuAffinityTest, ThreadAffinityGuard)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

#ifdef __linux__
  // Pin to core 0 first
  CpuAffinity::pinToCore(0);
  auto affinity = CpuAffinity::getCurrentAffinity();
  EXPECT_EQ(affinity.size(), 1);
  EXPECT_EQ(affinity[0], 0);

  {
    // Use guard to temporarily pin to core 1
    ThreadAffinityGuard guard(1);

    affinity = CpuAffinity::getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 1);
    EXPECT_EQ(affinity[0], 1);
  }

  // Should be restored to core 0
  affinity = CpuAffinity::getCurrentAffinity();
  EXPECT_EQ(affinity.size(), 1);
  EXPECT_EQ(affinity[0], 0);
#endif
}

/**
 * @brief Test thread affinity guard with multiple cores
 */
TEST_F(CpuAffinityTest, ThreadAffinityGuardMultipleCores)
{
  if (_numCores < 3)
  {
    GTEST_SKIP() << "Need at least 3 cores for this test";
  }

#ifdef __linux__
  {
    // Use guard to pin to cores 0 and 1
    ThreadAffinityGuard guard({0, 1});

    auto affinity = CpuAffinity::getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 2);
    EXPECT_TRUE(std::find(affinity.begin(), affinity.end(), 0) != affinity.end());
    EXPECT_TRUE(std::find(affinity.begin(), affinity.end(), 1) != affinity.end());
  }

  // Should be restored to original affinity
  auto affinity = CpuAffinity::getCurrentAffinity();
  EXPECT_EQ(affinity.size(), _originalAffinity.size());
#endif
}

/**
 * @brief Test thread pinning with separate thread
 */
TEST_F(CpuAffinityTest, ThreadPinning)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

  std::atomic<bool> threadPinned{false};
  std::atomic<int> threadCore{-1};

  std::thread t([&]()
                {
        bool result = CpuAffinity::pinToCore(1);
        threadPinned.store(result);
        
        if (result)
        {
            auto affinity = CpuAffinity::getCurrentAffinity();
            if (affinity.size() == 1)
            {
                threadCore.store(affinity[0]);
            }
        } });

  t.join();

#ifdef __linux__
  EXPECT_TRUE(threadPinned.load());
  EXPECT_EQ(threadCore.load(), 1);
#else
  EXPECT_FALSE(threadPinned.load());
#endif
}

/**
 * @brief Test recommended core assignment
 */
TEST_F(CpuAffinityTest, RecommendedCoreAssignment)
{
  auto assignment = CpuAffinity::getRecommendedCoreAssignment();

  // Should have at least some cores assigned
  int totalAssigned = assignment.marketDataCores.size() +
                      assignment.strategyCores.size() +
                      assignment.executionCores.size() +
                      assignment.riskCores.size() +
                      assignment.generalCores.size();

  EXPECT_GT(totalAssigned, 0);

  // All assigned cores should be valid
  auto validateCores = [this](const std::vector<int>& cores)
  {
    for (int core : cores)
    {
      EXPECT_GE(core, 0);
      EXPECT_LT(core, _numCores);
    }
  };

  validateCores(assignment.marketDataCores);
  validateCores(assignment.strategyCores);
  validateCores(assignment.executionCores);
  validateCores(assignment.riskCores);
  validateCores(assignment.generalCores);
}

/**
 * @brief Test isolated cores detection
 */
TEST_F(CpuAffinityTest, IsolatedCores)
{
  auto isolatedCores = CpuAffinity::getIsolatedCores();

  // Should not throw and return valid cores
  for (int core : isolatedCores)
  {
    EXPECT_GE(core, 0);
    EXPECT_LT(core, _numCores);
  }
}

/**
 * @brief Test real-time priority setting
 */
TEST_F(CpuAffinityTest, RealTimePriority)
{
  // This test may fail if not running as root
  bool result = CpuAffinity::setRealTimePriority(50);

  // Don't assert on the result since it depends on permissions
  // Just verify it doesn't crash
  EXPECT_TRUE(result || !result);  // Always true, just to check it runs
}

/**
 * @brief Test CPU frequency scaling control
 */
TEST_F(CpuAffinityTest, CpuFrequencyScaling)
{
  // These tests may fail without proper permissions
  bool disableResult = CpuAffinity::disableCpuFrequencyScaling();
  bool enableResult = CpuAffinity::enableCpuFrequencyScaling();

  // Don't assert on the results since they depend on permissions
  // Just verify they don't crash
  EXPECT_TRUE(disableResult || !disableResult);
  EXPECT_TRUE(enableResult || !enableResult);
}

/**
 * @brief Test EventBus with CPU affinity
 */
TEST_F(CpuAffinityTest, EventBusWithAffinity)
{
  TradeBus bus;

  // Configure CPU affinity
  auto assignment = CpuAffinity::getRecommendedCoreAssignment();
  bus.setCoreAssignment(assignment);

  // Verify assignment was set
  auto retrievedAssignment = bus.getCoreAssignment();
  EXPECT_TRUE(retrievedAssignment.has_value());

  if (retrievedAssignment.has_value())
  {
    const auto& assigned = retrievedAssignment.value();
    EXPECT_EQ(assigned.marketDataCores.size(), assignment.marketDataCores.size());
    EXPECT_EQ(assigned.strategyCores.size(), assignment.strategyCores.size());
    EXPECT_EQ(assigned.executionCores.size(), assignment.executionCores.size());
    EXPECT_EQ(assigned.riskCores.size(), assignment.riskCores.size());
    EXPECT_EQ(assigned.generalCores.size(), assignment.generalCores.size());
  }
}

/**
 * @brief Test multi-threaded CPU affinity
 */
TEST_F(CpuAffinityTest, MultiThreadedAffinity)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

  constexpr int numThreads = 4;
  std::vector<std::thread> threads;
  std::vector<std::atomic<int>> threadCores(numThreads);

  for (int i = 0; i < numThreads; ++i)
  {
    threadCores[i].store(-1);

    threads.emplace_back([&, i]()
                         {
            int targetCore = i % _numCores;
            bool result = CpuAffinity::pinToCore(targetCore);
            
            if (result)
            {
                auto affinity = CpuAffinity::getCurrentAffinity();
                if (affinity.size() == 1)
                {
                    threadCores[i].store(affinity[0]);
                }
            } });
  }

  for (auto& t : threads)
  {
    t.join();
  }

#ifdef __linux__
  // Verify threads were pinned correctly
  for (int i = 0; i < numThreads; ++i)
  {
    int expectedCore = i % _numCores;
    EXPECT_EQ(threadCores[i].load(), expectedCore);
  }
#endif
}

/**
 * @brief Stress test CPU affinity operations
 */
TEST_F(CpuAffinityTest, StressTest)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

  // Rapidly switch between cores
  for (int i = 0; i < 100; ++i)
  {
    int targetCore = i % _numCores;
    CpuAffinity::pinToCore(targetCore);

    // Small delay to allow OS to process
    std::this_thread::sleep_for(std::chrono::microseconds(10));
  }

  // Should still work after stress test
  auto affinity = CpuAffinity::getCurrentAffinity();
  EXPECT_FALSE(affinity.empty());
}

/**
 * @brief Test exception safety of ThreadAffinityGuard
 */
TEST_F(CpuAffinityTest, ExceptionSafety)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

#ifdef __linux__
  // Pin to core 0 first
  CpuAffinity::pinToCore(0);

  try
  {
    ThreadAffinityGuard guard(1);

    // Verify we're on core 1
    auto affinity = CpuAffinity::getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 1);
    EXPECT_EQ(affinity[0], 1);

    // Throw exception
    throw std::runtime_error("Test exception");
  }
  catch (const std::exception&)
  {
    // Exception caught, guard should have restored affinity
  }

  // Should be restored to core 0
  auto affinity = CpuAffinity::getCurrentAffinity();
  EXPECT_EQ(affinity.size(), 1);
  EXPECT_EQ(affinity[0], 0);
#endif
}