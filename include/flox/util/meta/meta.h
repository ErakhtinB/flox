/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <utility>

namespace flox::meta
{

template <auto Method>
struct WrapImpl;

template <typename Class, typename Ret, typename... Args, Ret (Class::*Method)(Args...)>
struct WrapImpl<Method>
{
  static constexpr Ret (*fn)(void*, Args...) = [](void* self, Args... args) -> Ret {
    return (static_cast<Class*>(self)->*Method)(std::forward<Args>(args)...);
  };
};

template <typename Class, typename Ret, typename... Args, Ret (Class::*Method)(Args...) const>
struct WrapImpl<Method>
{
  static constexpr Ret (*fn)(const void*, Args...) = [](const void* self, Args... args) -> Ret {
    return (static_cast<const Class*>(self)->*Method)(std::forward<Args>(args)...);
  };
};

template <auto Method>
constexpr auto wrap()
{
  return WrapImpl<Method>::fn;
}

}  // namespace flox::meta
