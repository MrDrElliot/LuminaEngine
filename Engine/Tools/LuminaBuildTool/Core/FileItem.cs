using System.Collections.Concurrent;

namespace LuminaBuildTool.Core;

/// <summary>
/// Interned file handle with a cached existence flag and timestamp. Every path the build graph
/// touches goes through here so a single build never stats the same file twice.
/// </summary>
public sealed class FileItem
{
    private static readonly ConcurrentDictionary<string, FileItem> Interned = new(StringComparer.OrdinalIgnoreCase);

    private bool bStatted;

    private bool bExists;

    private DateTime LastWriteTimeUtc;

    private long ByteLength;

    private FileItem(string AbsolutePath)
    {
        Location = AbsolutePath;
    }

    public string Location { get; }

    public string Name => Path.GetFileName(Location);

    public string Extension => Path.GetExtension(Location);

    public string Directory => Path.GetDirectoryName(Location) ?? string.Empty;

    public static FileItem Get(string AnyPath)
    {
        string Normalized = PathUtils.Normalize(AnyPath);
        return Interned.GetOrAdd(Normalized, static Key => new FileItem(Key));
    }

    public bool Exists
    {
        get
        {
            EnsureStatted();
            return bExists;
        }
    }

    /// <summary>
    /// Last write time, or DateTime.MinValue when the file is missing so a missing input always
    /// reads as older than any real output and never wrongly suppresses a rebuild.
    /// </summary>
    public DateTime Timestamp
    {
        get
        {
            EnsureStatted();
            return bExists ? LastWriteTimeUtc : DateTime.MinValue;
        }
    }

    public long Length
    {
        get
        {
            EnsureStatted();
            return bExists ? ByteLength : 0;
        }
    }

    /// <summary>
    /// Drops the cached stat after an action writes the file.
    /// </summary>
    public void Invalidate() => bStatted = false;

    public static void InvalidateAll()
    {
        foreach (FileItem Item in Interned.Values)
        {
            Item.Invalidate();
        }
    }

    private void EnsureStatted()
    {
        if (bStatted)
        {
            return;
        }

        FileInfo Info = new(Location);

        bExists = Info.Exists;
        LastWriteTimeUtc = bExists ? Info.LastWriteTimeUtc : DateTime.MinValue;
        ByteLength = bExists ? Info.Length : 0;
        bStatted = true;
    }

    public override string ToString() => Location;

    public override int GetHashCode() => StringComparer.OrdinalIgnoreCase.GetHashCode(Location);

    public override bool Equals(object? Other) => Other is FileItem Item && string.Equals(Location, Item.Location, StringComparison.OrdinalIgnoreCase);
}
