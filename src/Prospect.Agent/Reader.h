#pragma once

#include <Windows.h>
#include <cstdint>
#include <optional>
#include <type_traits>

template<typename T>
std::optional<T> TryRead(uintptr_t address)
{
    static_assert(std::is_trivially_copyable_v<T>);

    if (address == 0) {
        return std::nullopt;
    }

    T value{};
    SIZE_T bytesRead = 0;

    const BOOL success = ReadProcessMemory(
        GetCurrentProcess(),
        reinterpret_cast<const void*>(address),
        &value,
        sizeof(value),
        &bytesRead
    );

    if (!success || bytesRead != sizeof(value)) {
        return std::nullopt;
    }

    return value;
}