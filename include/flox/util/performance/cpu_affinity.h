/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace flox::performance
{

/**
 * @brief CPU affinity and thread pinning utilities for HFT performance optimization
 * 
 * This class provides functionality to:
 * - Pin threads to specific CPU cores
 * - Set thread priorities
 * - Isolate critical threads from OS interrupts
 * - Get CPU topology information
 */
class CpuAffinity
{
 public:
  /**
     * @brief Pin current thread to specific CPU core
     * @param coreId CPU core ID (0-based)
     * @return true if successful, false otherwise
     */
  static bool pinToCore(int coreId)
  {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);

    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0)
    {
      return true;
    }
    else
    {
      std::cerr << "[CpuAffinity] Failed to pin thread to core " << coreId
                << ": " << strerror(errno) << std::endl;
      return false;
    }
#else
    (void)coreId;
    std::cerr << "[CpuAffinity] CPU pinning not supported on this platform" << std::endl;
    return false;
#endif
  }

  /**
     * @brief Pin a thread to specific CPU core
     * @param thread Thread to pin
     * @param coreId CPU core ID (0-based)
     * @return true if successful, false otherwise
     */
  static bool pinToCore(std::thread& thread, int coreId)
  {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);

    if (pthread_setaffinity_np(thread.native_handle(), sizeof(cpu_set_t), &cpuset) == 0)
    {
      return true;
    }
    else
    {
      std::cerr << "[CpuAffinity] Failed to pin thread to core " << coreId
                << ": " << strerror(errno) << std::endl;
      return false;
    }
#else
    (void)thread;
    (void)coreId;
    std::cerr << "[CpuAffinity] CPU pinning not supported on this platform" << std::endl;
    return false;
#endif
  }

  /**
     * @brief Set thread priority for real-time performance
     * @param priority Priority level (1-99, higher = more priority)
     * @return true if successful, false otherwise
     */
  static bool setRealTimePriority(int priority = 80)
  {
#ifdef __linux__
    struct sched_param param;
    param.sched_priority = priority;

    if (sched_setscheduler(0, SCHED_FIFO, &param) == 0)
    {
      return true;
    }
    else
    {
      std::cerr << "[CpuAffinity] Failed to set real-time priority " << priority
                << ": " << strerror(errno) << std::endl;
      return false;
    }
#else
    (void)priority;
    std::cerr << "[CpuAffinity] Real-time priority not supported on this platform" << std::endl;
    return false;
#endif
  }

  /**
     * @brief Set thread priority for a specific thread
     * @param thread Thread to set priority for
     * @param priority Priority level (1-99, higher = more priority)
     * @return true if successful, false otherwise
     */
  static bool setRealTimePriority(std::thread& thread, int priority = 80)
  {
#ifdef __linux__
    struct sched_param param;
    param.sched_priority = priority;

    if (pthread_setschedparam(thread.native_handle(), SCHED_FIFO, &param) == 0)
    {
      return true;
    }
    else
    {
      std::cerr << "[CpuAffinity] Failed to set real-time priority " << priority
                << ": " << strerror(errno) << std::endl;
      return false;
    }
#else
    (void)thread;
    (void)priority;
    std::cerr << "[CpuAffinity] Real-time priority not supported on this platform" << std::endl;
    return false;
#endif
  }

  /**
     * @brief Get number of available CPU cores
     * @return Number of CPU cores
     */
  static int getNumCores()
  {
    return std::thread::hardware_concurrency();
  }

  /**
     * @brief Get list of isolated CPU cores (not used by OS)
     * @return Vector of isolated core IDs
     */
  static std::vector<int> getIsolatedCores()
  {
    std::vector<int> isolatedCores;

#ifdef __linux__
    // Read from /sys/devices/system/cpu/isolated
    auto content = readFile("/sys/devices/system/cpu/isolated");
    if (!content)
    {
      return isolatedCores;
    }

    std::stringstream ss(*content);
    std::string range;

    while (std::getline(ss, range, ','))
    {
      range.erase(range.find_last_not_of(" \n\r\t") + 1);
      range.erase(0, range.find_first_not_of(" \n\r\t"));

      if (range.empty())
        continue;

      size_t dashPos = range.find('-');
      if (dashPos != std::string::npos)
      {
        // Range like "2-5"
        int start = std::stoi(range.substr(0, dashPos));
        int end = std::stoi(range.substr(dashPos + 1));
        for (int i = start; i <= end; ++i)
        {
          isolatedCores.push_back(i);
        }
      }
      else
      {
        // Single core
        isolatedCores.push_back(std::stoi(range));
      }
    }
#endif

    return isolatedCores;
  }

  /**
     * @brief Get current thread's CPU affinity
     * @return Vector of CPU core IDs this thread can run on
     */
  static std::vector<int> getCurrentAffinity()
  {
    std::vector<int> affinity;

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    if (sched_getaffinity(0, sizeof(cpu_set_t), &cpuset) == 0)
    {
      for (int i = 0; i < CPU_SETSIZE; ++i)
      {
        if (CPU_ISSET(i, &cpuset))
        {
          affinity.push_back(i);
        }
      }
    }
#endif

    return affinity;
  }

  /**
     * @brief Disable CPU frequency scaling for performance cores
     * @return true if successful, false otherwise
     */
  static bool disableCpuFrequencyScaling()
  {
#ifdef __linux__
    // Set CPU governor to performance mode
    int numCores = getNumCores();
    bool success = true;

    for (int i = 0; i < numCores; ++i)
    {
      std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_governor";
      if (!writeFile(path, "performance"))
      {
        std::cerr << "[CpuAffinity] Failed to set performance governor for core " << i << std::endl;
        success = false;
      }
    }

    return success;
#else
    std::cerr << "[CpuAffinity] CPU frequency scaling control not supported on this platform" << std::endl;
    return false;
#endif
  }

  /**
     * @brief Enable CPU frequency scaling
     * @return true if successful, false otherwise
     */
  static bool enableCpuFrequencyScaling()
  {
#ifdef __linux__
    // Set CPU governor to ondemand mode
    int numCores = getNumCores();
    bool success = true;

    for (int i = 0; i < numCores; ++i)
    {
      std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_governor";
      if (!writeFile(path, "ondemand"))
      {
        std::cerr << "[CpuAffinity] Failed to set ondemand governor for core " << i << std::endl;
        success = false;
      }
    }

    return success;
#else
    std::cerr << "[CpuAffinity] CPU frequency scaling control not supported on this platform" << std::endl;
    return false;
#endif
  }

  /**
     * @brief Get recommended core assignment for HFT workloads
     * @return Struct containing recommended core assignments
     */
  struct CoreAssignment
  {
    std::vector<int> marketDataCores;  // Cores for market data processing
    std::vector<int> strategyCores;    // Cores for strategy execution
    std::vector<int> executionCores;   // Cores for order execution
    std::vector<int> riskCores;        // Cores for risk management
    std::vector<int> generalCores;     // Cores for general tasks
  };

  static CoreAssignment getRecommendedCoreAssignment()
  {
    CoreAssignment assignment;

    int numCores = getNumCores();
    auto isolatedCores = getIsolatedCores();

    if (isolatedCores.empty())
    {
      // No isolated cores, use all available cores
      for (int i = 0; i < numCores; ++i)
      {
        assignment.generalCores.push_back(i);
      }
    }
    else
    {
      // Use isolated cores for critical tasks
      int numIsolated = isolatedCores.size();

      if (numIsolated >= 4)
      {
        // Sufficient isolated cores for specialized assignment
        assignment.marketDataCores.push_back(isolatedCores[0]);
        assignment.strategyCores.push_back(isolatedCores[1]);
        assignment.executionCores.push_back(isolatedCores[2]);
        assignment.riskCores.push_back(isolatedCores[3]);

        // Remaining isolated cores for general use
        for (int i = 4; i < numIsolated; ++i)
        {
          assignment.generalCores.push_back(isolatedCores[i]);
        }
      }
      else
      {
        // Limited isolated cores, prioritize market data and execution
        if (numIsolated >= 1)
          assignment.marketDataCores.push_back(isolatedCores[0]);
        if (numIsolated >= 2)
          assignment.executionCores.push_back(isolatedCores[1]);
        if (numIsolated >= 3)
          assignment.strategyCores.push_back(isolatedCores[2]);
      }

      // Add non-isolated cores for general use
      for (int i = 0; i < numCores; ++i)
      {
        if (std::find(isolatedCores.begin(), isolatedCores.end(), i) == isolatedCores.end())
        {
          assignment.generalCores.push_back(i);
        }
      }
    }

    return assignment;
  }

 private:
  static std::optional<std::string> readFile(const std::string& path)
  {
    std::ifstream file(path);
    if (!file.is_open())
    {
      return std::nullopt;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  static bool writeFile(const std::string& path, const std::string& content)
  {
    std::ofstream file(path);
    if (!file.is_open())
    {
      return false;
    }

    file << content;
    return file.good();
  }
};

/**
 * @brief RAII wrapper for thread affinity management
 */
class ThreadAffinityGuard
{
 public:
  explicit ThreadAffinityGuard(int coreId)
  {
    _originalAffinity = CpuAffinity::getCurrentAffinity();
    CpuAffinity::pinToCore(coreId);
  }

  explicit ThreadAffinityGuard(const std::vector<int>& coreIds)
  {
    _originalAffinity = CpuAffinity::getCurrentAffinity();

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (int coreId : coreIds)
    {
      CPU_SET(coreId, &cpuset);
    }

    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
#else
    (void)coreIds;
#endif
  }

  ~ThreadAffinityGuard()
  {
    if (_restored || _originalAffinity.empty())
    {
      return;
    }

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (int coreId : _originalAffinity)
    {
      CPU_SET(coreId, &cpuset);
    }

    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
#endif

    _restored = true;
  }

  ThreadAffinityGuard(const ThreadAffinityGuard&) = delete;
  ThreadAffinityGuard& operator=(const ThreadAffinityGuard&) = delete;

 private:
  std::vector<int> _originalAffinity;
  bool _restored = false;
};

}  // namespace flox::performance