#pragma once

#include <cstdint>
#include <string>

// Contract: plain host-side traversal DTOs used to adapt discovery results
// into the Rust scan coordinator. This header intentionally contains no
// planning behavior; Rust is the source of truth for traversal work ownership.
struct TraversalChildCandidate
{
    std::wstring fullPath;
    std::uint32_t reparseTag = 0;
    bool isProtectedReparsePoint = false;
};

enum class TraversalSkipReason
{
    ProtectedReparsePoint,
    MountPointNotFollowed,
    SymbolicLinkNotFollowed,
    JunctionNotFollowed
};
