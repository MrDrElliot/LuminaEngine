namespace LuminaBuildTool.Core;

/// <summary>
/// Expected, user-facing build failure. Reported as a clean message with no stack trace.
/// </summary>
public sealed class BuildException : Exception
{
    public BuildException(string Message)
        : base(Message)
    {
    }

    public BuildException(string Format, params object[] Args)
        : base(string.Format(Format, Args))
    {
    }

    public BuildException(Exception Inner, string Message)
        : base(Message, Inner)
    {
    }
}
