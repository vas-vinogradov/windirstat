#pragma once

#include "DirectoryDiscoveryEngine.h"

// Temporary: adapts the Rust FFI discovery seam into the existing host
// DiscoveryBatch contract. It consumes only the requested path and returns the
// immediate children for that directory; traversal, scheduling, and UI/model
// projection stay outside this adapter.
// Removal condition: replace this bridge when the host contract is Rust-native
// and no longer needs the current DiscoveryBatch shape.
class RustReplacementDiscoveryEngine final : public IDirectoryDiscoveryEngine
{
public:
    DiscoveryBatch Discover(const DirectoryDiscoveryRequest& request) override;
};
