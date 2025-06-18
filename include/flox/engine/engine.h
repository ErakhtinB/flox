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
#include "flox/engine/abstract_engine.h"
#include "flox/engine/engine_config.h"
#include "flox/engine/subsystem.h"
#include "flox/util/error/error_system.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace flox
{

// Forward declarations
class ISubsystem;
class ExchangeConnector;

// Engine health status
struct EngineHealthStatus
{
  bool isHealthy = true;
  std::vector<FloxError> activeErrors;
  std::chrono::steady_clock::time_point lastUpdate;

  struct ComponentHealth
  {
    std::string name;
    bool isHealthy = true;
    std::optional<FloxError> lastError;
  };

  std::vector<ComponentHealth> components;
};

// Main Engine implementation with integrated error handling, health monitoring, and circuit breakers
class Engine : public IEngine
{
 public:
  Engine(const EngineConfig& config,
         std::vector<std::unique_ptr<ISubsystem>> subsystems = {},
         std::vector<std::shared_ptr<ExchangeConnector>> connectors = {});

  ~Engine() override;

  // IEngine implementation
  VoidResult start() override;
  VoidResult stop() override;
  bool isRunning() const noexcept override { return _running.load(); }

  Result<EngineHealthStatus> getHealthStatus() const override;
  VoidResult addSubsystem(std::unique_ptr<ISubsystem> subsystem) override;
  VoidResult addConnector(std::shared_ptr<ExchangeConnector> connector) override;
  Result<std::map<ErrorCategory, size_t>> getErrorStatistics() const override;

  // Error reporting and handling
  void reportError(const FloxError& error);
  void clearErrors(ErrorCategory category = {});

  // Circuit breaker functionality
  VoidResult executeWithCircuitBreaker(const std::string& operation,
                                       std::function<VoidResult()> func);

 private:
  // Validate engine state before operations
  VoidResult validateEngineState() const;

  // Start all subsystems with error handling
  VoidResult startSubsystems();

  // Start all connectors with error handling
  VoidResult startConnectors();

  // Stop subsystems in reverse order
  VoidResult stopSubsystems();

  // Stop all connectors
  VoidResult stopConnectors();

  // Update health status
  void updateHealthStatus();

  EngineConfig _config;
  std::vector<std::unique_ptr<ISubsystem>> _subsystems;
  std::vector<std::shared_ptr<ExchangeConnector>> _connectors;

  std::atomic<bool> _running{false};

  // Error tracking
  mutable std::shared_mutex _errorMutex;
  std::vector<FloxError> _errors;
  std::map<ErrorCategory, size_t> _errorCounts;

  // Health monitoring
  mutable std::shared_mutex _healthMutex;
  EngineHealthStatus _healthStatus;
  std::chrono::steady_clock::time_point _lastHealthUpdate;

  // Circuit breaker state
  struct CircuitBreakerState
  {
    std::atomic<int> failureCount{0};
    std::atomic<bool> isOpen{false};
    std::chrono::steady_clock::time_point lastFailureTime;
    static constexpr int FAILURE_THRESHOLD = 5;
    static constexpr std::chrono::seconds RECOVERY_TIMEOUT{30};
  };

  std::map<std::string, CircuitBreakerState> _circuitBreakers;
  mutable std::shared_mutex _circuitBreakerMutex;
};

// RAII Engine manager with automatic error handling
class EngineManager
{
 public:
  explicit EngineManager(std::unique_ptr<IEngine> engine)
      : _engine(std::move(engine))
  {
    if (auto result = _engine->start(); !result)
    {
      throw std::runtime_error("Failed to start engine: " + result.error().toString());
    }
  }

  ~EngineManager()
  {
    if (_engine && _engine->isRunning())
    {
      if (auto result = _engine->stop(); !result)
      {
        // Log error but don't throw in destructor
        // In production, this would go to a logger
      }
    }
  }

  // Non-copyable, moveable
  EngineManager(const EngineManager&) = delete;
  EngineManager& operator=(const EngineManager&) = delete;

  EngineManager(EngineManager&&) = default;
  EngineManager& operator=(EngineManager&&) = default;

  IEngine* operator->() { return _engine.get(); }
  const IEngine* operator->() const { return _engine.get(); }

  IEngine& operator*() { return *_engine; }
  const IEngine& operator*() const { return *_engine; }

 private:
  std::unique_ptr<IEngine> _engine;
};

// Builder pattern for engine construction
class EngineBuilder
{
 public:
  explicit EngineBuilder(const EngineConfig& config) : _config(config) {}

  // Add subsystem using template for type safety
  template <typename T, typename... Args>
    requires std::derived_from<T, ISubsystem>
  EngineBuilder& emplaceSubsystem(Args&&... args)
  {
    _subsystems.push_back(std::make_unique<T>(std::forward<Args>(args)...));
    return *this;
  }

  // Add connector
  EngineBuilder& addConnector(std::shared_ptr<ExchangeConnector> connector)
  {
    _connectors.push_back(std::move(connector));
    return *this;
  }

  // Build the engine
  Result<std::unique_ptr<Engine>> build()
  {
    auto engine = std::make_unique<Engine>(_config, std::move(_subsystems), std::move(_connectors));

    // Basic validation - the Engine constructor and start() method will do detailed validation
    if (_subsystems.empty() && _connectors.empty())
    {
      return std::unexpected(FLOX_ERROR_WARNING(CONFIG_VALIDATION_FAILED,
                                                "Engine has no subsystems or connectors configured"));
    }

    return std::move(engine);
  }

 private:
  EngineConfig _config;
  std::vector<std::unique_ptr<ISubsystem>> _subsystems;
  std::vector<std::shared_ptr<ExchangeConnector>> _connectors;
};

}  // namespace flox
