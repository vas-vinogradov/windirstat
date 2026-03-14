#pragma once
#include <string>
#include <cstdint>
#include <Windows.h>
struct DiscoveredFile {
    std::wstring name;       // File name
    std::wstring fullPath;   // Full path of the file
    uint64_t sizePhysical = 0; // Physical size of the file
    uint64_t sizeLogical = 0;  // Logical size of the file
    uint64_t index = 0;  // Logical size of the file
    FILETIME lastChange = {};  // Last modification time
    DWORD attributes = 0;     // File attributes
    DWORD reparseTag = 0;     // Reparse tag
    bool isReserved = false;  // Reserved flag
};
