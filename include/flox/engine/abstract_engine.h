/*
 * Flox Engine
 * Developed by Evgenii Makarov (https://github.com/eeiaao)
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/connector/exchange_connector.h"
#include "flox/engine/subsystem.h"
#include "flox/util/error/error_system.h"

#include <map>
#include <memory>

namespace flox
{

// Forward declarations
struct EngineHealthStatus;
class ExchangeConnector;

/**
 * @brief Abstract base interface for all engine implementations
 * 
 * This interface defines the core contract that all Flox engines must implement,
 * including integrated error handling, health monitoring, and lifecycle management.
 */
class IEngine
{
 public:
  virtual ~IEngine() = default;

  // Start engine with comprehensive error reporting
  virtual VoidResult start() = 0;

  // Stop engine with error reporting
  virtual VoidResult stop() = 0;

  // Check if engine is running
  virtual bool isRunning() const noexcept = 0;

  // Get current engine health status
  virtual Result<EngineHealthStatus> getHealthStatus() const = 0;

  // Add subsystem with validation
  virtual VoidResult addSubsystem(std::unique_ptr<ISubsystem> subsystem) = 0;

  // Add connector with validation
  virtual VoidResult addConnector(std::shared_ptr<ExchangeConnector> connector) = 0;

  // Get current error count by category
  virtual Result<std::map<ErrorCategory, size_t>> getErrorStatistics() const = 0;
};

}  // namespace flox
