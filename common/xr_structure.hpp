#pragma once

#include <openxr/openxr.h>

template<typename T>
constexpr T MakeXr(XrStructureType type) noexcept
{
    T value{};
    value.type = type;
    return value;
}
