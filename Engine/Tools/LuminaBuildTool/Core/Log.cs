namespace LuminaBuildTool.Core;

public enum LogLevel
{
    Trace,
    Verbose,
    Info,
    Warning,
    Error,
}

/// <summary>
/// Console logging. Writes to stderr for warnings and errors so tool output can be piped cleanly.
/// </summary>
public static class Log
{
    private static readonly object Gate = new();

    public static LogLevel MinLevel { get; set; } = LogLevel.Info;

    public static int WarningCount { get; private set; }

    public static int ErrorCount { get; private set; }

    public static void Trace(string Format, params object[] Args) => Write(LogLevel.Trace, Format, Args);

    public static void Verbose(string Format, params object[] Args) => Write(LogLevel.Verbose, Format, Args);

    public static void Info(string Format, params object[] Args) => Write(LogLevel.Info, Format, Args);

    public static void Warning(string Format, params object[] Args) => Write(LogLevel.Warning, Format, Args);

    public static void Error(string Format, params object[] Args) => Write(LogLevel.Error, Format, Args);

    /// <summary>
    /// Emits raw compiler or linker output verbatim so IDE error parsers still recognize it.
    /// </summary>
    public static void Raw(string Text)
    {
        if (string.IsNullOrWhiteSpace(Text))
        {
            return;
        }

        lock (Gate)
        {
            Console.Out.WriteLine(Text.TrimEnd());
        }
    }

    private static void Write(LogLevel Level, string Format, params object[] Args)
    {
        if (Level < MinLevel && Level < LogLevel.Warning)
        {
            return;
        }

        string Message = Args.Length > 0 ? string.Format(Format, Args) : Format;

        lock (Gate)
        {
            switch (Level)
            {
                case LogLevel.Warning:
                    WarningCount++;
                    WriteColored(ConsoleColor.Yellow, "warning: " + Message, Error: true);
                    break;

                case LogLevel.Error:
                    ErrorCount++;
                    WriteColored(ConsoleColor.Red, "error: " + Message, Error: true);
                    break;

                case LogLevel.Trace:
                case LogLevel.Verbose:
                    WriteColored(ConsoleColor.DarkGray, Message, Error: false);
                    break;

                default:
                    Console.Out.WriteLine(Message);
                    break;
            }
        }
    }

    private static void WriteColored(ConsoleColor Color, string Message, bool Error)
    {
        TextWriter Writer = Error ? Console.Error : Console.Out;
        ConsoleColor Previous = Console.ForegroundColor;

        try
        {
            Console.ForegroundColor = Color;
            Writer.WriteLine(Message);
        }
        finally
        {
            Console.ForegroundColor = Previous;
        }
    }
}
