#pragma once

#include "ScanTerminalReason.h"

#include <cstdint>
#include <optional>
#include <string>

// Contract: plain lifecycle DTOs for the scan-session state machine.
// Intent: keep completion, cancellation, failure, late-event, and terminal
// precedence rules expressible as data so this contract can later be
// implemented by the Rust scan engine without redesigning the host boundary.
struct ScanLifecycleFailure
{
    std::wstring path;
    std::wstring message;
    unsigned long errorCode = 0;
};

enum class ScanLifecycleTerminalKind
{
    Completed,
    Canceled,
    Failed
};

struct ScanLifecycleTerminalDecision
{
    ScanLifecycleTerminalKind kind = ScanLifecycleTerminalKind::Completed;
    ScanTerminalReason cancelReason = ScanTerminalReason::EngineInterrupted;
    ScanLifecycleFailure failure{};
};

struct ScanLifecycleSnapshot
{
    std::uint64_t outstandingWork = 0;
    bool inputClosed = false;
    bool cancelRequested = false;
    bool terminalReached = false;
    bool terminalDecisionTaken = false;
    ScanTerminalReason cancelReason = ScanTerminalReason::EngineInterrupted;
    std::optional<ScanLifecycleTerminalDecision> terminalDecision;
};
