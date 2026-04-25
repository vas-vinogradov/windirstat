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

Implemented prototype path: `NamedPipeScanClient` launches the existing scan host,
speaks the length-prefixed UTF-8 JSON named-pipe protocol, maps
`DirectoryProgressEvent` and `DirectoryProgressBatchEvent` into the C# DTOs, and
feeds `IScanObserver`. The mock feed remains available for UI-only iteration.

Next step: harden the C# client lifecycle and cancellation story before using it
for larger interactive scans.
