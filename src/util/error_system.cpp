/*
 * Flox Engine - Error System Implementation
 * Developed by Evgenii Makarov (https://github.com/eeiaao)
 *
 * Copyright (c) 2025 Evgenii Makarov
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/util/error/error_system.h"

namespace flox
{

// Global error statistics definitions
thread_local ErrorStats g_threadLocalErrorStats;
ErrorStats g_globalErrorStats;

}  // namespace flox