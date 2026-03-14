#pragma once
#include <string>
#include <cstdint>
#include <Windows.h>
struct DiscoveredDirectory {
    std::wstring name;       // Directory name
    std::wstring fullPath;   // Full path of the directory
    uint64_t index = 0;      // Index of the directory
    FILETIME lastChange = {}; // Last modification time
    DWORD attributes = 0;    // File attributes
    DWORD reparseTag = 0;    // Reparse tag
    bool isReserved = false; // Reserved flag
    bool isOffVolume = false; // Off-volume reparse point flag
    bool isProtectedReparsePoint = false; // Protected reparse point flag
};
