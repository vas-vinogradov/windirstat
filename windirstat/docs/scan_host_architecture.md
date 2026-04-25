# Scan Host Architecture

## Stable Shape

```text
UI/client -> NamedPipe RPC -> C++ scan-host adapter -> Rust scan engine
```

Rust owns scan logic: traversal, lifecycle, discovery, cancellation, completion,
progress scheduling, and worker concurrency. C++ owns transport: process launch,
named-pipe framing, RPC serialization, and projection of Rust-produced progress
events onto the existing client observer path.

The process boundary is RPC. The C++ scan host should stay an adapter around that
boundary, not a second scan engine.

## Current Baseline

The current real-app baseline uses `WinDirStat_x64.exe` with the RPC scan engine
and Rust scan threads set to 8. On the Projects tree this measured about 1.57s,
which is faster than the original local `/savetocsv` path at about 3.05s.

PowerShell RPC tools are useful for correctness and host-level experiments, but
they do not use the same pipe client as the app. Treat real app measurements from
`tools/app_rpc_scan_perf.ps1` as the baseline for user-visible performance.

## Experimental Transport Paths

The transport has three measured experimental paths:

- Host outbound queue: `WINDIRSTAT_RPC_OUTBOUND_QUEUE_CAPACITY`
- Client inbound queue: `WINDIRSTAT_RPC_INBOUND_QUEUE_CAPACITY`
- Directory progress batching: `WINDIRSTAT_RPC_BATCH_PROGRESS`

All are disabled by default. A missing env var or a zero queue capacity keeps the
synchronous RPC path. Measurements showed the experimental paths are correct, but
they did not improve the real app baseline; the synchronous path currently gives
the best overlap between Rust scanning and client observation.

Do not enable these paths in release builds by default. Use them only for explicit
experiments with the measurement scripts.

## Future Optimization Guidance

Preserve ownership first: Rust remains the scan engine and C++ remains transport.
Future performance work should start from real app measurements, then use host
tools to isolate a suspected bottleneck.

Good next-phase candidates are client/UI modernization, observer projection cost,
and reducing model/render work during high-volume progress. Avoid moving scan
logic back into MFC or C++ model objects to chase local wins.
