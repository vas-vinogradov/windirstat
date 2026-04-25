#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class UiScanEntryType : std::uint8_t
{
    File,
    Directory
};

struct UiScanEntryDto
{
    // Contract: name identifies the child inside its parent together with type.
    // fullPath is carried for future clients and validation; current MFC
    // projection reconstructs parent-child relationships from DirectoryResultDto::path.
    UiScanEntryType type = UiScanEntryType::File;
    std::wstring name;
    std::wstring fullPath;
    std::uint64_t sizeLogical = 0;
    std::uint64_t sizePhysical = 0;
    // File identity is best-effort. Some filesystems or compatibility paths may
    // report zero when no stable index is available; UIs must treat zero as
    // "unknown", not as a unique identity shared by all zero-index entries.
    std::uint64_t fileIndex = 0;
    std::uint64_t lastChangeFileTime = 0;
    std::uint32_t attributes = 0;
    std::uint32_t reparseTag = 0;
    bool isReserved = false;
    bool isOffVolume = false;
    bool isProtectedReparsePoint = false;
};

struct DirectoryResultDto
{
    std::uint64_t requestId = 0;
    // Contract: authoritative immediate-child snapshot for one directory path.
    // The UI observes this data only; traversal, lifecycle, and scheduling stay
    // owned by the scan engine.
    std::wstring path;
    // Contract: parent-before-child is the current projection assumption. The UI
    // must receive/process the DirectoryResultDto for a parent before DTOs for
    // nested child paths, otherwise the current MFC adapter cannot locate the
    // child parent item.
    std::vector<UiScanEntryDto> entries;
    // Contract: entries represent immediate children of path. Directory entries
    // should be projected before file entries; the mapper preserves this order
    // because the current CItem tree relies on directories existing before
    // follow-up child progress can be applied.
    //
    // Contract: requestId scopes every DTO. UI observers must ignore DTOs whose
    // requestId does not match the active scan request.
    //
    // Contract: finished=false is incremental/non-final. finished=true means no
    // more entries for this directory snapshot, so the UI may remove previously
    // projected children that are absent from entries.
    bool finished = false;
};
