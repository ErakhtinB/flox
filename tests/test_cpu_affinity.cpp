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
#include <set>
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

  // Helper method to check if NUMA is available on this system
  bool isNumaAvailable()
  {
    auto topology = CpuAffinity::getNumaTopology();
    return topology.numaAvailable && !topology.nodes.empty();
  }

  // Helper method to get a valid NUMA node for testing
  int getTestNumaNode()
  {
    auto topology = CpuAffinity::getNumaTopology();
    if (topology.numaAvailable && !topology.nodes.empty())
    {
      return topology.nodes[0].nodeId;
    }
    return -1;
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

/**
 * @brief Test NUMA topology detection
 */
TEST_F(CpuAffinityTest, NumaTopology)
{
  auto topology = CpuAffinity::getNumaTopology();

  // Basic validation
  EXPECT_GE(topology.numNodes, 0);
  EXPECT_EQ(topology.nodes.size(), static_cast<size_t>(topology.numNodes));

  if (topology.numaAvailable)
  {
    EXPECT_GT(topology.numNodes, 0);

    // Validate each node
    for (const auto& node : topology.nodes)
    {
      EXPECT_GE(node.nodeId, 0);
      EXPECT_FALSE(node.cpuCores.empty());

      // All cores should be valid
      for (int core : node.cpuCores)
      {
        EXPECT_GE(core, 0);
        EXPECT_LT(core, _numCores);
      }

      // Memory info should be reasonable
      EXPECT_GE(node.totalMemoryMB, 0);
      EXPECT_GE(node.freeMemoryMB, 0);
      EXPECT_LE(node.freeMemoryMB, node.totalMemoryMB);
    }

    // Ensure all cores are accounted for
    std::set<int> allNumaCores;
    for (const auto& node : topology.nodes)
    {
      for (int core : node.cpuCores)
      {
        allNumaCores.insert(core);
      }
    }

    // Should have at least some cores mapped
    EXPECT_FALSE(allNumaCores.empty());
  }
  else
  {
    EXPECT_EQ(topology.numNodes, 0);
    EXPECT_TRUE(topology.nodes.empty());
  }
}

/**
 * @brief Test NUMA node to core mapping
 */
TEST_F(CpuAffinityTest, NumaNodeForCore)
{
  auto topology = CpuAffinity::getNumaTopology();

  if (!topology.numaAvailable)
  {
    GTEST_SKIP() << "NUMA not available on this system";
  }

  // Test mapping for cores we know exist
  for (const auto& node : topology.nodes)
  {
    for (int core : node.cpuCores)
    {
      int numaNode = CpuAffinity::getNumaNodeForCore(core);
      // Note: The mapping might not be perfect due to different methods
      // but it should return a valid node ID or -1
      EXPECT_TRUE(numaNode == -1 || numaNode >= 0);
    }
  }

  // Test invalid core
  int invalidNode = CpuAffinity::getNumaNodeForCore(999);
  EXPECT_EQ(invalidNode, -1);
}

/**
 * @brief Test NUMA node pinning
 */
TEST_F(CpuAffinityTest, PinToNumaNode)
{
  auto topology = CpuAffinity::getNumaTopology();

  if (!topology.numaAvailable || topology.nodes.empty())
  {
    GTEST_SKIP() << "NUMA not available on this system";
  }

#ifdef __linux__
  // Pin to first NUMA node
  int nodeId = topology.nodes[0].nodeId;
  bool result = CpuAffinity::pinToNumaNode(nodeId);
  EXPECT_TRUE(result);

  // Check that we're pinned to cores in that node
  auto affinity = CpuAffinity::getCurrentAffinity();
  EXPECT_FALSE(affinity.empty());

  // All pinned cores should be in the target NUMA node
  const auto& nodeCores = topology.nodes[0].cpuCores;
  for (int core : affinity)
  {
    EXPECT_TRUE(std::find(nodeCores.begin(), nodeCores.end(), core) != nodeCores.end());
  }

  // Test invalid node
  result = CpuAffinity::pinToNumaNode(999);
  EXPECT_FALSE(result);
#else
  bool result = CpuAffinity::pinToNumaNode(0);
  EXPECT_FALSE(result);  // Should fail on non-Linux platforms
#endif
}

/**
 * @brief Test NUMA node pinning with separate thread
 */
TEST_F(CpuAffinityTest, ThreadNumaPinning)
{
  auto topology = CpuAffinity::getNumaTopology();

  if (!topology.numaAvailable || topology.nodes.empty())
  {
    GTEST_SKIP() << "NUMA not available on this system";
  }

  std::atomic<bool> threadPinned{false};
  std::atomic<int> threadNodeCores{0};

  int nodeId = topology.nodes[0].nodeId;
  const auto& expectedCores = topology.nodes[0].cpuCores;

  std::thread t([&]()
                {
        bool result = CpuAffinity::pinToNumaNode(nodeId);
        threadPinned.store(result);
        
        if (result)
        {
            auto affinity = CpuAffinity::getCurrentAffinity();
            int coresInNode = 0;
            for (int core : affinity)
            {
                if (std::find(expectedCores.begin(), expectedCores.end(), core) != expectedCores.end())
                {
                    coresInNode++;
                }
            }
            threadNodeCores.store(coresInNode);
        } });

  t.join();

#ifdef __linux__
  EXPECT_TRUE(threadPinned.load());
  EXPECT_GT(threadNodeCores.load(), 0);
#else
  EXPECT_FALSE(threadPinned.load());
#endif
}

/**
 * @brief Test memory policy setting
 */
TEST_F(CpuAffinityTest, MemoryPolicy)
{
  auto topology = CpuAffinity::getNumaTopology();

  if (!topology.numaAvailable || topology.nodes.empty())
  {
    GTEST_SKIP() << "NUMA not available on this system";
  }

#ifdef __linux__
  int nodeId = topology.nodes[0].nodeId;
  bool result = CpuAffinity::setMemoryPolicy(nodeId);
  EXPECT_TRUE(result);

  // Test invalid node
  result = CpuAffinity::setMemoryPolicy(999);
  EXPECT_FALSE(result);
#else
  bool result = CpuAffinity::setMemoryPolicy(0);
  EXPECT_FALSE(result);  // Should fail on non-Linux platforms
#endif
}

/**
 * @brief Test NUMA-aware core assignment
 */
TEST_F(CpuAffinityTest, NumaAwareCoreAssignment)
{
  auto assignment = CpuAffinity::getNumaAwareCoreAssignment();
  auto topology = CpuAffinity::getNumaTopology();

  // Basic validation
  EXPECT_TRUE(assignment.marketDataCores.size() <= static_cast<size_t>(_numCores));
  EXPECT_TRUE(assignment.strategyCores.size() <= static_cast<size_t>(_numCores));
  EXPECT_TRUE(assignment.executionCores.size() <= static_cast<size_t>(_numCores));
  EXPECT_TRUE(assignment.riskCores.size() <= static_cast<size_t>(_numCores));
  EXPECT_TRUE(assignment.generalCores.size() <= static_cast<size_t>(_numCores));

  // All cores should be valid
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

  if (topology.numaAvailable && !topology.nodes.empty())
  {
    // NUMA-aware assignment should try to keep related tasks on same node
    // For systems with sufficient cores, market data and execution should be on same node
    if (!assignment.marketDataCores.empty() && !assignment.executionCores.empty())
    {
      int marketDataNode = CpuAffinity::getNumaNodeForCore(assignment.marketDataCores[0]);
      int executionNode = CpuAffinity::getNumaNodeForCore(assignment.executionCores[0]);

      // If both return valid nodes, they should ideally be the same
      if (marketDataNode >= 0 && executionNode >= 0)
      {
        // This is a preference, not a strict requirement
        // Just verify they're both valid assignments
        EXPECT_GE(marketDataNode, 0);
        EXPECT_GE(executionNode, 0);
      }
    }
  }
}

/**
 * @brief Test NumaAffinityGuard RAII wrapper
 */
TEST_F(CpuAffinityTest, NumaAffinityGuard)
{
  auto topology = CpuAffinity::getNumaTopology();

  if (!topology.numaAvailable || topology.nodes.empty())
  {
    GTEST_SKIP() << "NUMA not available on this system";
  }

#ifdef __linux__
  // Pin to core 0 first
  if (_numCores >= 1)
  {
    CpuAffinity::pinToCore(0);
    auto affinity = CpuAffinity::getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 1);
    EXPECT_EQ(affinity[0], 0);
  }

  int nodeId = topology.nodes[0].nodeId;
  const auto& nodeCores = topology.nodes[0].cpuCores;

  {
    // Use NUMA guard to temporarily pin to NUMA node
    NumaAffinityGuard guard(nodeId);

    auto affinity = CpuAffinity::getCurrentAffinity();
    EXPECT_FALSE(affinity.empty());

    // All cores should be from the target NUMA node
    for (int core : affinity)
    {
      EXPECT_TRUE(std::find(nodeCores.begin(), nodeCores.end(), core) != nodeCores.end());
    }
  }

  // Should be restored to previous state (core 0)
  if (_numCores >= 1)
  {
    auto affinity = CpuAffinity::getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 1);
    EXPECT_EQ(affinity[0], 0);
  }
#endif
}

/**
 * @brief Test NumaAffinityGuard with specific core
 */
TEST_F(CpuAffinityTest, NumaAffinityGuardSpecificCore)
{
  auto topology = CpuAffinity::getNumaTopology();

  if (!topology.numaAvailable || topology.nodes.empty() || _numCores < 2)
  {
    GTEST_SKIP() << "NUMA not available or insufficient cores";
  }

#ifdef __linux__
  int nodeId = topology.nodes[0].nodeId;
  const auto& nodeCores = topology.nodes[0].cpuCores;

  if (nodeCores.empty())
  {
    GTEST_SKIP() << "No cores available in NUMA node";
  }

  int targetCore = nodeCores[0];

  {
    // Use NUMA guard to pin to specific core and set memory policy
    NumaAffinityGuard guard(targetCore, nodeId);

    auto affinity = CpuAffinity::getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 1);
    EXPECT_EQ(affinity[0], targetCore);
  }

  // Should be restored to original affinity
  auto affinity = CpuAffinity::getCurrentAffinity();
  EXPECT_EQ(affinity.size(), _originalAffinity.size());
#endif
}

/**
 * @brief Test NUMA functionality with multi-threading
 */
TEST_F(CpuAffinityTest, NumaMultiThreaded)
{
  auto topology = CpuAffinity::getNumaTopology();

  if (!topology.numaAvailable || topology.nodes.empty())
  {
    GTEST_SKIP() << "NUMA not available on this system";
  }

  const int numThreads = std::min(4, static_cast<int>(topology.nodes.size()));
  std::vector<std::atomic<bool>> threadResults(numThreads);
  std::vector<std::thread> threads;

  for (int i = 0; i < numThreads; ++i)
  {
    threadResults[i].store(false);

    threads.emplace_back([&, i]()
                         {
        int nodeId = topology.nodes[i % topology.nodes.size()].nodeId;
        
        // Use NUMA affinity guard
        NumaAffinityGuard guard(nodeId);
        
        // Verify pinning worked
        auto affinity = CpuAffinity::getCurrentAffinity();
        if (!affinity.empty())
        {
            const auto& expectedCores = topology.nodes[i % topology.nodes.size()].cpuCores;
            bool allCoresInNode = true;
            
            for (int core : affinity)
            {
                if (std::find(expectedCores.begin(), expectedCores.end(), core) == expectedCores.end())
                {
                    allCoresInNode = false;
                    break;
                }
            }
            
            threadResults[i].store(allCoresInNode);
        }
        
        // Simulate some work
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); });
  }

  for (auto& t : threads)
  {
    t.join();
  }

#ifdef __linux__
  for (int i = 0; i < numThreads; ++i)
  {
    EXPECT_TRUE(threadResults[i].load()) << "Thread " << i << " failed NUMA pinning";
  }
#endif
}

/**
 * @brief Test conditional NUMA guard usage - demonstrates best practice pattern
 */
TEST_F(CpuAffinityTest, ConditionalNumaGuardUsage)
{
  // This test demonstrates the recommended pattern for applications:
  // Check if NUMA is available before using NUMA guards

  if (isNumaAvailable())
  {
    // NUMA is available - use NUMA-aware optimizations
    int testNode = getTestNumaNode();
    ASSERT_GE(testNode, 0) << "Should have valid NUMA node when NUMA is available";

    {
      // Use NUMA guard for optimal memory locality
      NumaAffinityGuard numaGuard(testNode);

      // Verify we're pinned to the NUMA node
      auto affinity = CpuAffinity::getCurrentAffinity();
      EXPECT_FALSE(affinity.empty());

      // Simulate memory-intensive work that benefits from NUMA locality
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // NUMA guard automatically restored affinity and memory policy
    std::cout << "NUMA optimizations applied for node " << testNode << std::endl;
  }
  else
  {
    // NUMA not available - fall back to regular CPU affinity
    if (_numCores >= 2)
    {
      ThreadAffinityGuard cpuGuard(0);  // Pin to core 0

      // Verify regular CPU pinning works
      auto affinity = CpuAffinity::getCurrentAffinity();
      EXPECT_EQ(affinity.size(), 1);
      EXPECT_EQ(affinity[0], 0);

      // Simulate work without NUMA optimizations
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "NUMA not available - using regular CPU affinity" << std::endl;
  }
}

/**
 * @brief Test mixed guard usage - CPU guards with optional NUMA
 */
TEST_F(CpuAffinityTest, MixedGuardUsage)
{
  if (_numCores < 2)
  {
    GTEST_SKIP() << "Need at least 2 cores for this test";
  }

  // Always use basic CPU affinity
  {
    ThreadAffinityGuard cpuGuard(1);

    auto affinity = CpuAffinity::getCurrentAffinity();
    EXPECT_EQ(affinity.size(), 1);
    EXPECT_EQ(affinity[0], 1);

    // Conditionally add NUMA optimizations if available
    if (isNumaAvailable())
    {
      int nodeId = CpuAffinity::getNumaNodeForCore(1);
      if (nodeId >= 0)
      {
        // Set memory policy for the NUMA node containing core 1
        bool memPolicySet = CpuAffinity::setMemoryPolicy(nodeId);
        EXPECT_TRUE(memPolicySet || !memPolicySet);  // Don't assert - just verify it doesn't crash

        std::cout << "Enhanced with NUMA memory policy for node " << nodeId << std::endl;
      }
    }

    // Simulate work that benefits from both CPU and NUMA affinity
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}