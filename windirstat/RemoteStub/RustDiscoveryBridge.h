#pragma once

#include <cstddef>
#include <cstdint>

enum class RustDiscoveryEntryKind : std::uint32_t
{
    File = 0,
    Directory = 1
};

struct RustDiscoveryEntryView
{
    RustDiscoveryEntryKind kind = RustDiscoveryEntryKind::File;
    const std::uint16_t* name = nullptr;
    std::size_t nameLen = 0;
    const std::uint16_t* fullPath = nullptr;
    std::size_t fullPathLen = 0;
    std::uint64_t sizePhysical = 0;
    std::uint64_t sizeLogical = 0;
    std::uint64_t index = 0;
    std::uint64_t lastWriteTime = 0;
    std::uint32_t attributes = 0;
    std::uint32_t reparseTag = 0;
    std::uint8_t isReserved = 0;
    std::uint8_t isOffVolume = 0;
    std::uint8_t isProtectedReparsePoint = 0;
};

struct RustDiscoveryErrorView
{
    const char* message = nullptr;
    std::size_t messageLen = 0;
};

using RustDiscoveryEntryCallback = void(*)(const RustDiscoveryEntryView* entry, void* userData);
using RustDiscoveryLogCallback = void(*)(const std::uint16_t* message, std::size_t messageLen, void* userData);

extern "C"
{
    bool windirstat_rust_discover_directory(
        const std::uint16_t* path,
        std::size_t pathLen,
        RustDiscoveryEntryCallback callback,
        void* userData,
        RustDiscoveryLogCallback logCallback,
        void* logUserData,
        RustDiscoveryErrorView* error);
}
