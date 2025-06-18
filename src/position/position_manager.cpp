/*
 * Flox Engine
 * Developed by Evgenii Makarov (https://github.com/eeiaao)
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/position/position_manager.h"
#include <algorithm>
#include <numeric>

namespace flox
{

PositionManager::PositionManager(SubscriberId id, PositionConfig config)
    : IOrderExecutionListener(id), _config(std::move(config))
{
}

// ISubsystem interface implementations - keep void return types
void PositionManager::start()
{
  auto result = startInternal();
  if (!result)
  {
    // Report error but maintain interface contract
    reportErrorFast(result.error().code(), result.error().severity());
    // Could throw here in debug mode or log to external system
  }
}

void PositionManager::stop()
{
  auto result = stopInternal();
  if (!result)
  {
    // Report error but maintain interface contract
    reportErrorFast(result.error().code(), result.error().severity());
  }
}

// IOrderExecutionListener interface implementations - keep void return types
void PositionManager::onOrderAccepted(const Order& order)
{
  auto result = onOrderAcceptedInternal(order);
  if (!result)
  {
    reportErrorFast(result.error().code(), result.error().severity());
  }
}

void PositionManager::onOrderPartiallyFilled(const Order& order, Quantity fillQty)
{
  auto result = onOrderPartiallyFilledInternal(order, fillQty);
  if (!result)
  {
    reportErrorFast(result.error().code(), result.error().severity());
    // For critical position updates, trigger emergency stop
    if (result.error().severity() >= ErrorSeverity::CRITICAL)
    {
      if (_killSwitch)
      {
        std::string reason = std::format("Critical position tracking error: {} (Order: {}, Fill: {})",
                                         result.error().message(), order.id, fillQty);
        _killSwitch->trigger(reason);
      }
    }
  }
}

void PositionManager::onOrderFilled(const Order& order)
{
  auto result = onOrderFilledInternal(order);
  if (!result)
  {
    reportErrorFast(result.error().code(), result.error().severity());
    // Critical error in position tracking - this is serious
    if (result.error().severity() >= ErrorSeverity::CRITICAL)
    {
      if (_killSwitch)
      {
        std::string reason = std::format("Critical position tracking error on order fill: {} (Order: {}, Qty: {})",
                                         result.error().message(), order.id, order.quantity);
        _killSwitch->trigger(reason);
      }
    }
  }
}

void PositionManager::onOrderCanceled(const Order& order)
{
  auto result = onOrderCanceledInternal(order);
  if (!result)
  {
    reportErrorFast(result.error().code(), result.error().severity());
  }
}

void PositionManager::onOrderExpired(const Order& order)
{
  auto result = onOrderExpiredInternal(order);
  if (!result)
  {
    reportErrorFast(result.error().code(), result.error().severity());
  }
}

void PositionManager::onOrderRejected(const Order& order)
{
  auto result = onOrderRejectedInternal(order);
  if (!result)
  {
    reportErrorFast(result.error().code(), result.error().severity());
  }
}

void PositionManager::onOrderReplaced(const Order& oldOrder, const Order& newOrder)
{
  auto result = onOrderReplacedInternal(oldOrder, newOrder);
  if (!result)
  {
    reportErrorFast(result.error().code(), result.error().severity());
  }
}

// IPositionManager interface implementation - keep original return type
Quantity PositionManager::getPosition(SymbolId symbol) const
{
  auto result = getPositionInternal(symbol);
  if (!result)
  {
    reportErrorFast(result.error().code(), result.error().severity());
    return Quantity{0};  // Return zero position on error
  }
  return result.value();
}

// Internal error-aware implementations
VoidResult PositionManager::startInternal()
{
  std::unique_lock lock(_statsMutex);
  if (_isRunning)
  {
    return std::unexpected(expandError(ErrorCode::ENGINE_ALREADY_RUNNING, "PositionManager is already running"));
  }

  _isRunning = true;
  _stats = PositionStats{};  // Reset stats
  return VoidResult{};
}

VoidResult PositionManager::stopInternal()
{
  std::unique_lock lock(_statsMutex);
  if (!_isRunning)
  {
    return std::unexpected(expandError(ErrorCode::ENGINE_NOT_STARTED, "PositionManager is not running"));
  }

  _isRunning = false;
  return VoidResult{};
}

VoidResult PositionManager::onOrderAcceptedInternal(const Order& order)
{
  std::shared_lock lock(_positionMutex);
  return VoidResult{};  // Order accepted doesn't change position
}

VoidResult PositionManager::onOrderFilledInternal(const Order& order)
{
  Quantity deltaQuantity = (order.side == Side::BUY) ? order.quantity : -order.quantity;
  return updatePositionInternal(order.symbol, deltaQuantity, std::format("Order {} filled ({})", order.id, order.quantity));
}

VoidResult PositionManager::onOrderPartiallyFilledInternal(const Order& order, Quantity qty)
{
  Quantity deltaQuantity = (order.side == Side::BUY) ? qty : -qty;
  return updatePositionInternal(order.symbol, deltaQuantity, std::format("Order {} partially filled ({})", order.id, qty));
}

VoidResult PositionManager::onOrderCanceledInternal(const Order& order)
{
  std::shared_lock lock(_positionMutex);
  return VoidResult{};  // Order canceled doesn't change position
}

VoidResult PositionManager::onOrderExpiredInternal(const Order& order)
{
  std::shared_lock lock(_positionMutex);
  return VoidResult{};  // Order expired doesn't change position
}

VoidResult PositionManager::onOrderRejectedInternal(const Order& order)
{
  std::shared_lock lock(_positionMutex);
  return VoidResult{};  // Order rejected doesn't change position
}

VoidResult PositionManager::onOrderReplacedInternal(const Order& oldOrder, const Order& newOrder)
{
  std::shared_lock lock(_positionMutex);
  return VoidResult{};  // Order replacement handled separately
}

Result<Quantity> PositionManager::getPositionInternal(SymbolId symbol) const
{
  std::shared_lock lock(_positionMutex);

  auto it = _positions.find(symbol);
  if (it == _positions.end())
  {
    return Quantity{0};  // No position means zero
  }

  return it->second;
}

VoidResult PositionManager::validatePositionLimits(SymbolId symbol, Quantity newQuantity) const
{
  if (!_config.enableRiskChecks)
  {
    return VoidResult{};
  }

  // Check individual position limit
  if (newQuantity.abs() > _config.maxPositionSize)
  {
    return std::unexpected(FloxError::Critical(ErrorCode::RISK_POSITION_LIMIT_EXCEEDED,
                                               std::format("Position {} exceeds maximum size limit {}",
                                                           newQuantity, _config.maxPositionSize)));
  }

  // Check total exposure limit
  Quantity totalExposure = newQuantity.abs();
  std::shared_lock lock(_positionMutex);
  for (const auto& [sym, pos] : _positions)
  {
    if (sym != symbol)
    {
      totalExposure += pos.abs();
    }
  }

  if (totalExposure > _config.maxTotalExposure)
  {
    return std::unexpected(FloxError::Critical(ErrorCode::RISK_CONCENTRATION_LIMIT_EXCEEDED,
                                               std::format("Total exposure {} exceeds maximum limit {}",
                                                           totalExposure, _config.maxTotalExposure)));
  }

  return VoidResult{};
}

VoidResult PositionManager::updatePosition(SymbolId symbol, Quantity quantity)
{
  return updatePositionInternal(symbol, quantity, "Manual position update");
}

VoidResult PositionManager::resetPosition(SymbolId symbol)
{
  std::unique_lock lock(_positionMutex);
  _positions.erase(symbol);
  return VoidResult{};
}

VoidResult PositionManager::resetAllPositions()
{
  std::unique_lock lock(_positionMutex);
  _positions.clear();
  return VoidResult{};
}

Result<Quantity> PositionManager::getTotalExposure() const
{
  std::shared_lock lock(_positionMutex);

  Quantity totalExposure{0};
  for (const auto& [symbol, position] : _positions)
  {
    totalExposure += position.abs();
  }

  return totalExposure;
}

Result<PositionStats> PositionManager::getStats() const
{
  std::lock_guard lock(_statsMutex);
  return _stats;
}

VoidResult PositionManager::updatePositionInternal(SymbolId symbol, Quantity deltaQuantity, const std::string& reason)
{
  // First, get current position and calculate new position
  Quantity currentPosition{0};
  Quantity newPosition;

  {
    std::shared_lock posLock(_positionMutex);
    auto it = _positions.find(symbol);
    if (it != _positions.end())
    {
      currentPosition = it->second;
    }
    newPosition = currentPosition + deltaQuantity;
  }

  // Validate new position OUTSIDE of the mutex to avoid deadlock
  if (auto validationResult = validatePositionLimits(symbol, newPosition); !validationResult)
  {
    // Trigger emergency stop for critical risk violations
    if (validationResult.error().severity() >= ErrorSeverity::CRITICAL)
    {
      triggerEmergencyStop(std::format("Position limit violation: {} (Symbol: {}, New Position: {}, Reason: {})",
                                       validationResult.error().message(), symbol, newPosition, reason));
    }
    return validationResult;
  }

  // Now acquire unique lock and update position
  {
    std::unique_lock posLock(_positionMutex);

    // Re-check current position in case it changed between locks
    auto it = _positions.find(symbol);
    Quantity actualCurrentPosition{0};
    if (it != _positions.end())
    {
      actualCurrentPosition = it->second;
    }

    // If position changed, recalculate and re-validate
    if (actualCurrentPosition != currentPosition)
    {
      newPosition = actualCurrentPosition + deltaQuantity;
      posLock.unlock();

      // Re-validate with the actual current position
      if (auto validationResult = validatePositionLimits(symbol, newPosition); !validationResult)
      {
        // Trigger emergency stop for critical risk violations
        if (validationResult.error().severity() >= ErrorSeverity::CRITICAL)
        {
          triggerEmergencyStop(std::format("Position limit violation on re-validation: {} (Symbol: {}, New Position: {}, Reason: {})",
                                           validationResult.error().message(), symbol, newPosition, reason));
        }
        return validationResult;
      }

      posLock.lock();
    }

    // Update position
    if (newPosition.isZero())
    {
      _positions.erase(symbol);  // Remove zero positions
    }
    else
    {
      _positions[symbol] = newPosition;
    }
  }

  // Update statistics
  {
    std::lock_guard statsLock(_statsMutex);
    _stats.totalTrades++;
    _stats.maxPositionReached = std::max(_stats.maxPositionReached, newPosition.abs());
    _stats.lastTradeTime = std::chrono::system_clock::now();
  }

  // Record in history
  {
    std::lock_guard historyLock(_historyMutex);
    _positionHistory.push_back(newPosition);

    // Limit history size
    if (_positionHistory.size() > 1000)
    {
      _positionHistory.erase(_positionHistory.begin(), _positionHistory.begin() + 100);
    }
  }

  return VoidResult{};
}

void PositionManager::triggerEmergencyStop(const std::string& reason)
{
  if (_killSwitch && !_killSwitch->isTriggered())
  {
    _killSwitch->trigger(reason);

    // Log the emergency stop
    auto criticalError = FloxError::Fatal(ErrorCode::RISK_POSITION_LIMIT_EXCEEDED,
                                          std::format("EMERGENCY STOP TRIGGERED: {}", reason));
    reportErrorFast(criticalError.code(), criticalError.severity());
  }
}

}  // namespace flox
