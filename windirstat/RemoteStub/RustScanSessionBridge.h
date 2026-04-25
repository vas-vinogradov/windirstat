#pragma once

#include "RemoteStub/RustScanEngineBridge.h"

#include <cstddef>
#include <cstdint>

// Intent: C ABI for the Rust-owned scan session.
// Contract: Rust owns traversal progression, immediate-child discovery, and
// lifecycle decisions; the host remains responsible for RPC emission and
// request-input handling.
// Temporary: this bridge exists while the C++ scan host is still the process
// boundary shell around the Rust engine.
// Removal condition: replace this C ABI with the final RPC boundary once the
// scan engine runs as its own Rust process.
struct RustScanSession;

enum class RustScanSessionTerminalReason : std::uint32_t
{
    UserCancel = 1,
    Restarted = 2,
    EngineInterrupted = 3
};

enum class RustScanSessionTerminalKind : std::uint32_t
{
    Completed = 0,
    Canceled = 1,
    Failed = 2
};

struct RustScanSessionOptionsView
{
    std::uint8_t followMountPoints = 0;
    std::uint8_t followSymbolicLinks = 0;
    std::uint8_t followJunctions = 0;
};

struct RustScanSessionChildCandidateView
{
    const std::uint16_t* fullPath = nullptr;
    std::size_t fullPathLen = 0;
    std::uint32_t reparseTag = 0;
    std::uint8_t isProtectedReparsePoint = 0;
};

struct RustScanSessionPathView
{
    const std::uint16_t* path = nullptr;
    std::size_t pathLen = 0;
};

enum class RustScanSessionActionKind : std::uint32_t
{
    RequestDirectory = 0,
    EmitDirectoryResult = 1,
    WaitingForInput = 2,
    Complete = 3,
    Cancelled = 4,
    Failed = 5
};

struct RustScanSessionFailureView
{
    const std::uint16_t* path = nullptr;
    std::size_t pathLen = 0;
    const std::uint16_t* message = nullptr;
    std::size_t messageLen = 0;
    std::uint32_t errorCode = 0;
};

struct RustScanSessionTerminalDecisionView
{
    RustScanSessionTerminalKind kind = RustScanSessionTerminalKind::Completed;
    RustScanSessionTerminalReason cancelReason = RustScanSessionTerminalReason::EngineInterrupted;
    RustScanSessionFailureView failure{};
};

struct RustScanSessionActionView
{
    RustScanSessionActionKind kind = RustScanSessionActionKind::WaitingForInput;
    RustScanSessionPathView path{};
    std::size_t scheduledChildCount = 0;
    RustScanSessionTerminalReason cancelReason = RustScanSessionTerminalReason::EngineInterrupted;
    RustScanSessionFailureView failure{};
};

struct RustScanSessionSnapshotView
{
    std::uint64_t outstandingWork = 0;
    std::uint8_t inputClosed = 0;
    std::uint8_t cancelRequested = 0;
    std::uint8_t terminalReached = 0;
    std::uint8_t terminalDecisionTaken = 0;
    RustScanSessionTerminalReason cancelReason = RustScanSessionTerminalReason::EngineInterrupted;
    std::uint8_t hasTerminalDecision = 0;
    RustScanSessionTerminalDecisionView terminalDecision{};
};

struct RustScanSessionTimingStatsView
{
    std::uint64_t discoveryWorkerElapsedNs = 0;
    std::uint64_t schedulingElapsedNs = 0;
    std::uint64_t noEventPollCount = 0;
    std::size_t workerCount = 0;
};

using RustScanSessionPathCallback = void(*)(const RustScanSessionPathView* path, void* userData);

extern "C"
{
    RustScanSession* windirstat_rust_scan_session_create();
    void windirstat_rust_scan_session_destroy(RustScanSession* session);
    bool windirstat_rust_scan_session_start(
        RustScanSession* session,
        const std::uint16_t* rootPath,
        std::size_t rootPathLen,
        RustScanSessionOptionsView options);
    void windirstat_rust_scan_session_reset(RustScanSession* session);
    void windirstat_rust_scan_session_clear_pending_work(RustScanSession* session);
    bool windirstat_rust_scan_session_add_external_path(
        RustScanSession* session,
        const std::uint16_t* path,
        std::size_t pathLen);
    bool windirstat_rust_scan_session_close_input(RustScanSession* session);
    bool windirstat_rust_scan_session_request_cancel(
        RustScanSession* session,
        RustScanSessionTerminalReason reason);
    bool windirstat_rust_scan_session_next_action(
        RustScanSession* session,
        RustScanSessionActionView* output,
        RustDiscoveryEntryCallback entryCallback,
        void* userData);
    bool windirstat_rust_scan_session_take_next_path(
        RustScanSession* session,
        RustScanSessionPathCallback callback,
        void* userData);
    bool windirstat_rust_scan_session_report_directory_result(
        RustScanSession* session,
        const RustScanSessionChildCandidateView* childCandidates,
        std::size_t childCandidateCount,
        RustScanSessionPathCallback scheduledCallback,
        void* userData);
    bool windirstat_rust_scan_session_report_directory_failure(
        RustScanSession* session,
        RustScanSessionFailureView failure);
    bool windirstat_rust_scan_session_has_terminal_decision(const RustScanSession* session);
    bool windirstat_rust_scan_session_take_terminal_decision(
        RustScanSession* session,
        RustScanSessionTerminalDecisionView* output);
    bool windirstat_rust_scan_session_snapshot(
        const RustScanSession* session,
        RustScanSessionSnapshotView* output);
    std::size_t windirstat_rust_scan_session_pending_count(const RustScanSession* session);
    bool windirstat_rust_scan_session_is_done(const RustScanSession* session);
    bool windirstat_rust_scan_session_timing_stats(
        const RustScanSession* session,
        RustScanSessionTimingStatsView* output);
}
