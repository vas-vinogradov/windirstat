#pragma once

#include <cstdint>

enum class ScanTerminalReason : std::uint8_t
{
    UserCancel = 1,
    Restarted = 2,
    EngineInterrupted = 3
};
