#pragma once
#include "Reader.h"

void RequestIntroSkip()
{
    constexpr uintptr_t MoviePlayerOffset = 0x6591A38;
    constexpr uintptr_t StopMovieOffset = 0x359D600;
    using StopMovieFunction = void (*)(void*);

    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    const auto moviePlayer = TryRead<uintptr_t>(base + MoviePlayerOffset);
    const auto stopMovie = reinterpret_cast<StopMovieFunction>(base + StopMovieOffset);

    stopMovie(reinterpret_cast<void*>(*moviePlayer));
}