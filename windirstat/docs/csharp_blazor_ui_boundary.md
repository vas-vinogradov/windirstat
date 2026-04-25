# C# / Blazor UI Boundary

## Contract

The C# prototype mirrors the C++ `DirectoryResultDto` and `UiScanEntryDto`
contracts without adding UI-only fields. The DTOs are JSON-ready POCOs under
`ui/WinDirStat.Blazor/Contracts`.

The UI observes scan results through `IScanObserver`:

```csharp
void OnDirectoryResult(DirectoryResultDto dto);
void OnScanCompleted(ulong requestId);
void OnScanFailed(ulong requestId, string error);
```

The observer owns projection only. Traversal, lifecycle, cancellation, discovery,
and concurrency remain owned by the Rust scan engine behind the RPC process
boundary.

## Projection

`ScanTreeProjection` is intentionally much smaller than `CItem`. It exists to
prove the DTO contract can build a UI-consumable tree:

- ignores stale request IDs
- applies directory entries before file entries
- reconciles missing children only when `Finished == true`
- keeps file and directory keys distinct

This is not a replacement for the MFC model.

## Transport Options

1. Reuse the existing named-pipe JSON protocol from C#.
2. Wrap that protocol in a thin C# client that emits `DirectoryResultDto` to
   `IScanObserver`.
3. Later, consider a gRPC or HTTP bridge if the host boundary changes.

Recommended next step: implement a thin C# named-pipe JSON client. It can consume
the existing `RpcDirectoryProgressEvent` stream and map it into the same C# DTOs
without changing the Rust engine, C++ host, or RPC protocol.
