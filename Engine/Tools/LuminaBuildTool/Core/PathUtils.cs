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
    public static bool WriteFileIfChanged(string FilePath, string Content)
    {
        string Normalized = Normalize(FilePath);

        if (File.Exists(Normalized) && File.ReadAllText(Normalized) == Content)
        {
            return false;
        }

        EnsureDirectoryForFile(Normalized);
        File.WriteAllText(Normalized, Content, new UTF8Encoding(encoderShouldEmitUTF8Identifier: true));
        FileItem.Get(Normalized).Invalidate();

        return true;
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
