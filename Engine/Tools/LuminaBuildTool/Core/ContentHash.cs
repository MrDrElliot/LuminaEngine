using System.Security.Cryptography;
using System.Text;

namespace LuminaBuildTool.Core;

/// <summary>Stable content hashing for cache keys.</summary>
public static class ContentHash
{
    public static string OfString(string Value)
    {
        byte[] Digest = SHA256.HashData(Encoding.UTF8.GetBytes(Value));
        return Convert.ToHexString(Digest, 0, 12).ToLowerInvariant();
    }

    /// <summary>Length-prefixes each element so no combination of values can collide by concatenation.</summary>
    public static string OfStrings(IEnumerable<string> Values)
    {
        StringBuilder Builder = new();

        foreach (string Value in Values)
        {
            Builder.Append(Value.Length).Append(':').Append(Value);
        }

        return OfString(Builder.ToString());
    }

    /// <summary>Hashes a file's bytes. Use when identity must survive a timestamp change.</summary>
    public static string OfFileContents(string FilePath)
    {
        using FileStream Stream = File.OpenRead(FilePath);
        return Convert.ToHexString(SHA256.HashData(Stream), 0, 12).ToLowerInvariant();
    }

    public static string OfFiles(IEnumerable<string> FilePaths)
    {
        List<string> Parts = new();

        foreach (string FilePath in FilePaths.OrderBy(P => P, StringComparer.OrdinalIgnoreCase))
        {
            FileItem Item = FileItem.Get(FilePath);
            Parts.Add($"{Item.Location}|{Item.Timestamp.Ticks}|{Item.Length}");
        }

        return OfStrings(Parts);
    }
}
