using System.Text;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>Work an action performs in process instead of by launching a tool.</summary>
public abstract class BuildOperation
{
    /// <summary>Stable identity of the work, folded into the command key so a change forces a rerun.</summary>
    public abstract string GetIdentity();

    /// <summary>Performs the work. Throws on failure; the executor decides how to report it.</summary>
    public abstract void Execute();
}

/// <summary>Stages a prebuilt file next to the build's output.</summary>
public sealed class CopyFileOperation : BuildOperation
{
    public CopyFileOperation(string Source, string Destination)
    {
        this.Source = Source;
        this.Destination = Destination;
    }

    public string Source { get; }

    public string Destination { get; }

    public override string GetIdentity() => $"copy:{Source}=>{Destination}";

    public override void Execute()
    {
        // Vendored dependencies commonly arrive read-only from an archive, and the attribute
        // travels with the copy, so a second build cannot overwrite its own staged output.
        ClearReadOnly(Destination);

        File.Copy(Source, Destination, overwrite: true);

        ClearReadOnly(Destination);
    }

    /// <summary>True when the destination already matches the source byte for byte.</summary>
    public bool IsAlreadyStaged()
    {
        FileItem SourceItem = FileItem.Get(Source);
        FileItem DestinationItem = FileItem.Get(Destination);

        if (!DestinationItem.Exists || SourceItem.Length != DestinationItem.Length)
        {
            return false;
        }

        try
        {
            return ContentHash.OfFileContents(Source) == ContentHash.OfFileContents(Destination);
        }
        catch (IOException)
        {
            return false;
        }
    }

    private static void ClearReadOnly(string FilePath)
    {
        if (!File.Exists(FilePath))
        {
            return;
        }

        FileAttributes Attributes = File.GetAttributes(FilePath);

        if ((Attributes & FileAttributes.ReadOnly) != 0)
        {
            File.SetAttributes(FilePath, Attributes & ~FileAttributes.ReadOnly);
        }
    }
}

/// <summary>Materializes a generated text file later actions consume, such as a generator's input.</summary>
public sealed class WriteFileOperation : BuildOperation
{
    public WriteFileOperation(string Destination, string Contents)
    {
        this.Destination = Destination;
        this.Contents = Contents;
    }

    public string Destination { get; }

    public string Contents { get; }

    // The content is the identity: rewriting the same bytes is not work worth doing.
    public override string GetIdentity() => $"write:{Destination}:{ContentHash.OfString(Contents)}";

    public override void Execute()
    {
        PathUtils.EnsureDirectoryForFile(Destination);
        File.WriteAllText(Destination, Contents, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
    }
}
