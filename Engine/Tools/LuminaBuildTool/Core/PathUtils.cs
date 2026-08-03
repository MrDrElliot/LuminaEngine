using System.Text;

namespace LuminaBuildTool.Core;

public static class PathUtils
{
    /// <summary>
    /// Absolute, full path with native separators. All build-graph paths are normalized through
    /// here so two spellings of the same file never produce two distinct graph nodes.
    /// </summary>
    public static string Normalize(string AnyPath)
    {
        if (string.IsNullOrEmpty(AnyPath))
        {
            return string.Empty;
        }

        string Full = Path.GetFullPath(AnyPath);
        return Full.Length > 3 ? Full.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) : Full;
    }

    public static string Combine(string Base, params string[] Parts)
    {
        string Result = Base;

        foreach (string Part in Parts)
        {
            Result = Path.Combine(Result, Part);
        }

        return Normalize(Result);
    }

    public static string MakeRelativeTo(string AbsolutePath, string BaseDirectory)
    {
        try
        {
            return Path.GetRelativePath(BaseDirectory, AbsolutePath);
        }
        catch (ArgumentException)
        {
            return AbsolutePath;
        }
    }

    public static void EnsureDirectory(string DirectoryPath)
    {
        if (!string.IsNullOrEmpty(DirectoryPath))
        {
            System.IO.Directory.CreateDirectory(DirectoryPath);
        }
    }

    public static void EnsureDirectoryForFile(string FilePath)
    {
        EnsureDirectory(Path.GetDirectoryName(FilePath) ?? string.Empty);
    }

    /// <summary>Whether two paths name the same file, ignoring separator and case differences.</summary>
    public static bool AreSame(string Left, string Right)
    {
        return string.Equals(Normalize(Left), Normalize(Right), StringComparison.OrdinalIgnoreCase);
    }

    public static bool IsUnder(string CandidatePath, string DirectoryPath)
    {
        string Candidate = Normalize(CandidatePath);
        string Root = Normalize(DirectoryPath);

        return Candidate.StartsWith(Root, StringComparison.OrdinalIgnoreCase)
            && (Candidate.Length == Root.Length || Candidate[Root.Length] == Path.DirectorySeparatorChar);
    }

    /// <summary>
    /// Writes only when content differs so generated project files keep stable timestamps and
    /// do not retrigger downstream work on every generation pass.
    /// </summary>
    /// <param name="bByteOrderMark">
    /// MSBuild's own tooling writes project files with a byte order mark, so they keep one. JSON
    /// must not have one: the grammar has no place for it and a strict parser stops at the first
    /// character. Anything consumed by something other than Visual Studio wants it off.
    /// </param>
    public static bool WriteFileIfChanged(string FilePath, string Content, bool bByteOrderMark = true)
    {
        string Normalized = Normalize(FilePath);

        if (IsAlreadyWritten(Normalized, Content, bByteOrderMark))
        {
            return false;
        }

        EnsureDirectoryForFile(Normalized);
        File.WriteAllText(Normalized, Content, new UTF8Encoding(encoderShouldEmitUTF8Identifier: bByteOrderMark));
        FileItem.Get(Normalized).Invalidate();

        return true;
    }

    /// <summary>
    /// Whether the file already holds exactly this text in the requested encoding.
    /// </summary>
    /// <remarks>
    /// ReadAllText strips a byte order mark if there is one, so content alone cannot tell us the
    /// encoding matches. Both have to, or a file whose text never changes would keep whichever
    /// encoding it was first written with forever. An unreadable file counts as not written, which
    /// lets the write attempt surface the real error rather than this check swallowing it.
    /// </remarks>
    private static bool IsAlreadyWritten(string FilePath, string Content, bool bByteOrderMark)
    {
        try
        {
            if (!File.Exists(FilePath))
            {
                return false;
            }

            byte[] Head = new byte[3];
            int Read;

            using (FileStream Stream = File.OpenRead(FilePath))
            {
                Read = Stream.Read(Head, 0, Head.Length);
            }

            bool bFound = Read == 3 && Head[0] == 0xEF && Head[1] == 0xBB && Head[2] == 0xBF;

            return bFound == bByteOrderMark && File.ReadAllText(FilePath) == Content;
        }
        catch (Exception Ex) when (Ex is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    /// <summary>
    /// Quotes an argument for a Windows command line, escaping embedded quotes and trailing slashes.
    /// </summary>
    public static string Quote(string Argument)
    {
        if (Argument.Length > 0 && Argument.IndexOfAny(new[] { ' ', '\t', '"' }) < 0)
        {
            return Argument;
        }

        StringBuilder Builder = new(Argument.Length + 8);
        Builder.Append('"');

        int Backslashes = 0;

        foreach (char Character in Argument)
        {
            if (Character == '\\')
            {
                Backslashes++;
                continue;
            }

            if (Character == '"')
            {
                Builder.Append('\\', (Backslashes * 2) + 1);
                Backslashes = 0;
            }
            else
            {
                Builder.Append('\\', Backslashes);
                Backslashes = 0;
            }

            Builder.Append(Character);
        }

        Builder.Append('\\', Backslashes * 2);
        Builder.Append('"');

        return Builder.ToString();
    }
}
