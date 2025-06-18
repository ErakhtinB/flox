/*
 * Flox Engine
 * Developed by Evgenii Makarov (https://github.com/eeiaao)
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/engine/engine.h"
#include "flox/engine/engine_config.h"

#include <algorithm>
#include <format>
#include <mutex>

namespace flox
{

Engine::Engine(const EngineConfig& config,
               std::vector<std::unique_ptr<ISubsystem>> subsystems,
               std::vector<std::shared_ptr<ExchangeConnector>> connectors)
    : _config(config), _subsystems(std::move(subsystems)), _connectors(std::move(connectors)), _lastHealthUpdate(std::chrono::steady_clock::now())
{
  updateHealthStatus();
}

Engine::~Engine()
{
  if (_running.load())
  {
    stop();  // Ensure clean shutdown
  }
}

VoidResult Engine::start()
{
  if (auto validation = validateEngineState(); !validation)
  {
    return validation;
  }

  if (_running.exchange(true))
  {
    return std::unexpected(FLOX_ERROR(ENGINE_ALREADY_RUNNING, "Engine is already running"));
  }

  // Start connectors first
  if (auto result = startConnectors(); !result)
  {
    _running.store(false);
    return result;
  }

  // Then start subsystems
  if (auto result = startSubsystems(); !result)
  {
    stopConnectors();  // Cleanup on failure
    _running.store(false);
    return result;
  }

  updateHealthStatus();
  return {};
}

VoidResult Engine::stop()
{
  if (!_running.exchange(false))
  {
    return std::unexpected(FLOX_ERROR_WARNING(ENGINE_NOT_STARTED, "Engine is not running"));
  }

  auto subsystemResult = stopSubsystems();
  auto connectorResult = stopConnectors();

  updateHealthStatus();

  // Return first error if any occurred
  if (!subsystemResult)
  {
    return subsystemResult;
  }
  if (!connectorResult)
  {
    return connectorResult;
  }

  return {};
}

Result<EngineHealthStatus> Engine::getHealthStatus() const
{
  std::shared_lock lock(_healthMutex);
  return _healthStatus;
}

VoidResult Engine::addSubsystem(std::unique_ptr<ISubsystem> subsystem)
{
  if (!subsystem)
  {
    return std::unexpected(FLOX_ERROR(VALIDATION_REQUIRED_FIELD_MISSING, "Subsystem cannot be null"));
  }

  if (_running.load())
  {
    return std::unexpected(FLOX_ERROR(ENGINE_ALREADY_RUNNING, "Cannot add subsystems while engine is running"));
  }

  _subsystems.push_back(std::move(subsystem));
  return {};
}

VoidResult Engine::addConnector(std::shared_ptr<ExchangeConnector> connector)
{
  if (!connector)
  {
    return std::unexpected(FLOX_ERROR(VALIDATION_REQUIRED_FIELD_MISSING, "Connector cannot be null"));
  }

  if (_running.load())
  {
    return std::unexpected(FLOX_ERROR(ENGINE_ALREADY_RUNNING, "Cannot add connectors while engine is running"));
  }

  _connectors.push_back(std::move(connector));
  return {};
}

Result<std::map<ErrorCategory, size_t>> Engine::getErrorStatistics() const
{
  std::shared_lock lock(_errorMutex);
  return _errorCounts;
}

void Engine::reportError(const FloxError& error)
{
  std::unique_lock lock(_errorMutex);

  _errors.push_back(error);
  _errorCounts[error.category()]++;

  // Limit error history to prevent unbounded growth
  if (_errors.size() > 1000)
  {
    _errors.erase(_errors.begin(), _errors.begin() + 100);
  }

  updateHealthStatus();
}

void Engine::clearErrors(ErrorCategory category)
{
  std::unique_lock lock(_errorMutex);

  if (category == ErrorCategory{})
  {
    // Clear all errors
    _errors.clear();
    _errorCounts.clear();
  }
  else
  {
    // Clear specific category
    _errors.erase(
        std::remove_if(_errors.begin(), _errors.end(),
                       [category](const FloxError& err)
                       { return err.category() == category; }),
        _errors.end());
    _errorCounts[category] = 0;
  }

  updateHealthStatus();
}

VoidResult Engine::executeWithCircuitBreaker(const std::string& operation,
                                             std::function<VoidResult()> func)
{
  std::unique_lock lock(_circuitBreakerMutex);
  auto& breaker = _circuitBreakers[operation];

  // Check if circuit breaker is open
  if (breaker.isOpen.load())
  {
    auto now = std::chrono::steady_clock::now();
    if (now - breaker.lastFailureTime < Engine::CircuitBreakerState::RECOVERY_TIMEOUT)
    {
      return std::unexpected(FLOX_ERROR(ENGINE_SHUTDOWN_FAILED,
                                        std::format("Circuit breaker open for operation: {}", operation)));
    }
    else
    {
      // Reset circuit breaker after timeout
      breaker.isOpen.store(false);
      breaker.failureCount.store(0);
    }
  }

  lock.unlock();

  // Execute the operation
  auto result = func();

  lock.lock();
  if (result)
  {
    // Success - reset failure count
    breaker.failureCount.store(0);
  }
  else
  {
    // Failure - increment count and possibly open circuit
    auto failures = breaker.failureCount.fetch_add(1) + 1;
    if (failures >= Engine::CircuitBreakerState::FAILURE_THRESHOLD)
    {
      breaker.isOpen.store(true);
      breaker.lastFailureTime = std::chrono::steady_clock::now();
    }
  }

  return result;
}

VoidResult Engine::validateEngineState() const
{
  if (_subsystems.empty() && _connectors.empty())
  {
    return std::unexpected(FLOX_ERROR_WARNING(CONFIG_VALIDATION_FAILED,
                                              "Engine has no subsystems or connectors configured"));
  }

  return {};
}

VoidResult Engine::startSubsystems()
{
  for (size_t i = 0; i < _subsystems.size(); ++i)
  {
    try
    {
      _subsystems[i]->start();
    }
    catch (const std::exception& e)
    {
      // Cleanup already started subsystems
      for (size_t j = 0; j < i; ++j)
      {
        try
        {
          _subsystems[j]->stop();
        }
        catch (...)
        {
          // Log but don't throw in cleanup
        }
      }

      return std::unexpected(FLOX_ERROR(SUBSYSTEM_INIT_FAILED,
                                        std::format("Failed to start subsystem {}: {}", i, e.what())));
    }
  }

  return {};
}

VoidResult Engine::startConnectors()
{
  for (size_t i = 0; i < _connectors.size(); ++i)
  {
    try
    {
      _connectors[i]->start();
    }
    catch (const std::exception& e)
    {
      // Cleanup already started connectors
      for (size_t j = 0; j < i; ++j)
      {
        try
        {
          _connectors[j]->stop();
        }
        catch (...)
        {
          // Log but don't throw in cleanup
        }
      }

      return std::unexpected(FLOX_ERROR(CONNECTION_AUTH_FAILED,
                                        std::format("Failed to start connector {}: {}", i, e.what())));
    }
  }

  return {};
}

VoidResult Engine::stopSubsystems()
{
  VoidResult result;

  // Stop in reverse order
  for (auto it = _subsystems.rbegin(); it != _subsystems.rend(); ++it)
  {
    try
    {
      (*it)->stop();
    }
    catch (const std::exception& e)
    {
      if (result)
      {  // Only capture first error
        result = std::unexpected(FLOX_ERROR(ENGINE_SHUTDOWN_FAILED,
                                            std::format("Failed to stop subsystem: {}", e.what())));
      }
    }
  }

  return result;
}

VoidResult Engine::stopConnectors()
{
  VoidResult result;

  for (auto& connector : _connectors)
  {
    try
    {
      connector->stop();
    }
    catch (const std::exception& e)
    {
      if (result)
      {  // Only capture first error
        result = std::unexpected(FLOX_ERROR(ENGINE_SHUTDOWN_FAILED,
                                            std::format("Failed to stop connector: {}", e.what())));
      }
    }
  }

  return result;
}

void Engine::updateHealthStatus()
{
  std::unique_lock lock(_healthMutex);

  _healthStatus.lastUpdate = std::chrono::steady_clock::now();

  // Check if we have any critical or fatal errors
  std::shared_lock errorLock(_errorMutex);
  _healthStatus.activeErrors.clear();

  for (const auto& error : _errors)
  {
    if (error.severity() >= ErrorSeverity::CRITICAL)
    {
      _healthStatus.activeErrors.push_back(error);
    }
  }

  _healthStatus.isHealthy = _healthStatus.activeErrors.empty() && _running.load();

  // Update component health (simplified for now)
  _healthStatus.components.clear();

  for (size_t i = 0; i < _subsystems.size(); ++i)
  {
    EngineHealthStatus::ComponentHealth component;
    component.name = std::format("Subsystem_{}", i);
    component.isHealthy = true;  // TODO: Implement actual health checks
    _healthStatus.components.push_back(component);
  }

  for (size_t i = 0; i < _connectors.size(); ++i)
  {
    EngineHealthStatus::ComponentHealth component;
    component.name = std::format("Connector_{}", i);
    component.isHealthy = true;  // TODO: Implement actual health checks
    _healthStatus.components.push_back(component);
  }
}

}  // namespace flox