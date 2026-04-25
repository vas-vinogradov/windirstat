using WinDirStat.Blazor.Contracts;

namespace WinDirStat.Blazor.Projection;

public sealed class ScanTreeProjection : IScanObserver
{
    public ulong ActiveRequestId { get; private set; }
    public ScanNode? Root { get; private set; }
    public string? LastError { get; private set; }
    public bool Completed { get; private set; }

    public void StartRequest(ulong requestId)
    {
        ActiveRequestId = requestId;
        Root = null;
        LastError = null;
        Completed = false;
    }

    public void OnDirectoryResult(DirectoryResultDto dto)
    {
        if (dto.RequestId != ActiveRequestId)
            return;

        if (string.IsNullOrWhiteSpace(dto.Path))
            return;

        var parent = EnsureDirectory(dto.Path);
        var presentKeys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        foreach (var entry in dto.Entries.Where(e => e.Type == EntryType.Directory))
        {
            ApplyEntry(parent, entry);
            presentKeys.Add(Key(entry));
        }

        foreach (var entry in dto.Entries.Where(e => e.Type == EntryType.File))
        {
            ApplyEntry(parent, entry);
            presentKeys.Add(Key(entry));
        }

        if (dto.Finished)
        {
            foreach (var staleKey in parent.Children.Keys.Where(key => !presentKeys.Contains(key)).ToArray())
                parent.Children.Remove(staleKey);
        }

        Recalculate(parent);
    }

    public void OnScanCompleted(ulong requestId)
    {
        if (requestId == ActiveRequestId)
            Completed = true;
    }

    public void OnScanFailed(ulong requestId, string error)
    {
        if (requestId == ActiveRequestId)
            LastError = error;
    }

    private ScanNode EnsureDirectory(string path)
    {
        if (Root is null)
        {
            Root = new ScanNode
            {
                Name = System.IO.Path.GetFileName(path.TrimEnd(System.IO.Path.DirectorySeparatorChar, System.IO.Path.AltDirectorySeparatorChar)),
                Path = path,
                IsDirectory = true
            };
        }

        if (string.Equals(Root.Path, path, StringComparison.OrdinalIgnoreCase))
            return Root;

        var current = Root;
        var relative = path[Root.Path.Length..].TrimStart(System.IO.Path.DirectorySeparatorChar, System.IO.Path.AltDirectorySeparatorChar);
        foreach (var segment in relative.Split([System.IO.Path.DirectorySeparatorChar, System.IO.Path.AltDirectorySeparatorChar], StringSplitOptions.RemoveEmptyEntries))
        {
            var childPath = System.IO.Path.Combine(current.Path, segment);
            var key = DirectoryKey(segment);
            if (!current.Children.TryGetValue(key, out var next))
            {
                next = new ScanNode { Name = segment, Path = childPath, IsDirectory = true };
                current.Children[key] = next;
            }

            current = next;
        }

        return current;
    }

    private static void ApplyEntry(ScanNode parent, ScanEntryDto entry)
    {
        parent.Children[Key(entry)] = new ScanNode
        {
            Name = entry.Name,
            Path = entry.FullPath,
            IsDirectory = entry.Type == EntryType.Directory,
            SizeLogical = entry.SizeLogical,
            SizePhysical = entry.SizePhysical,
            FileIndex = entry.FileIndex
        };
    }

    private static void Recalculate(ScanNode node)
    {
        foreach (var child in node.Children.Values.Where(child => child.IsDirectory))
            Recalculate(child);

        if (node.IsDirectory)
        {
            node.SizeLogical = (ulong)node.Children.Values.Sum(child => (decimal)child.SizeLogical);
            node.SizePhysical = (ulong)node.Children.Values.Sum(child => (decimal)child.SizePhysical);
        }
    }

    private static string Key(ScanEntryDto entry) =>
        entry.Type == EntryType.Directory ? DirectoryKey(entry.Name) : $"F:{entry.Name}";

    private static string DirectoryKey(string name) => $"D:{name}";
}
