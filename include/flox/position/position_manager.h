/*
 * Flox Engine
 * Developed by Evgenii Makarov (https://github.com/eeiaao)
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include "flox/common.h"
#include "flox/engine/subsystem.h"
#include "flox/execution/abstract_execution_listener.h"
#include "flox/killswitch/abstract_killswitch.h"
#include "flox/position/abstract_position_manager.h"
#include "flox/util/error/error_system.h"

namespace flox
{

struct PositionConfig
{
  Quantity maxPositionSize{Quantity::fromDouble(1000000.0)};
  Quantity maxTotalExposure{Quantity::fromDouble(10000000.0)};
  bool enableRiskChecks{true};
  double maxDrawdownPercent{0.1};
};

struct PositionStats
{
  size_t totalTrades{0};
  Quantity maxPositionReached{Quantity::fromDouble(0.0)};
  double totalPnL{0.0};
  double maxDrawdown{0.0};
  std::chrono::system_clock::time_point lastTradeTime;
};

class PositionManager : public IPositionManager, public IOrderExecutionListener, public ISubsystem
{
 public:
  explicit PositionManager(SubscriberId id, PositionConfig config = {});
  virtual ~PositionManager() = default;

  // ISubsystem interface - keep original void return types
  void start() override;
  void stop() override;

  // IOrderExecutionListener interface - keep original void return types
  void onOrderAccepted(const Order& order) override;
  void onOrderPartiallyFilled(const Order& order, Quantity fillQty) override;
  void onOrderFilled(const Order& order) override;
  void onOrderCanceled(const Order& order) override;
  void onOrderExpired(const Order& order) override;
  void onOrderRejected(const Order& order) override;
  void onOrderReplaced(const Order& oldOrder, const Order& newOrder) override;

  // IPositionManager interface - keep original return type
  Quantity getPosition(SymbolId symbol) const override;

  // New error-aware methods
  VoidResult startInternal();
  VoidResult stopInternal();
  VoidResult onOrderAcceptedInternal(const Order& order);
  VoidResult onOrderPartiallyFilledInternal(const Order& order, Quantity fillQty);
  VoidResult onOrderFilledInternal(const Order& order);
  VoidResult onOrderCanceledInternal(const Order& order);
  VoidResult onOrderExpiredInternal(const Order& order);
  VoidResult onOrderRejectedInternal(const Order& order);
  VoidResult onOrderReplacedInternal(const Order& oldOrder, const Order& newOrder);
  Result<Quantity> getPositionInternal(SymbolId symbol) const;

  // Additional functionality
  Result<Quantity> getTotalExposure() const;
  Result<PositionStats> getStats() const;
  VoidResult updatePosition(SymbolId symbol, Quantity quantity);
  VoidResult resetPosition(SymbolId symbol);
  VoidResult resetAllPositions();

  // Killswitch integration
  void setKillSwitch(IKillSwitch* killSwitch) { _killSwitch = killSwitch; }

 private:
  PositionConfig _config;
  mutable std::shared_mutex _positionMutex;
  mutable std::mutex _historyMutex;
  mutable std::mutex _statsMutex;

  std::unordered_map<SymbolId, Quantity> _positions;
  std::vector<Quantity> _positionHistory;
  PositionStats _stats;
  bool _isRunning{false};
  IKillSwitch* _killSwitch{nullptr};

  VoidResult validatePositionLimits(SymbolId symbol, Quantity newQuantity) const;
  VoidResult updatePositionInternal(SymbolId symbol, Quantity deltaQuantity, const std::string& reason);
  void triggerEmergencyStop(const std::string& reason);
};

}  // namespace flox
