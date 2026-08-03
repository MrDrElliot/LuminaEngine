using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Execution;

/// <summary>One executed action, as a span on the build's wall clock.</summary>
public readonly record struct TimelineSpan(
    string Name,
    string Category,
    TimeSpan Start,
    TimeSpan Duration,
    bool bSucceeded);

/// <summary>
/// Records when each action ran and for how long, and writes it as a trace a profiler can draw.
/// </summary>
/// <remarks>
/// Totals already tell you a build took thirty seconds. They cannot tell you that twenty of those
/// had one core busy because everything was waiting behind the reflection generator, or that one
/// unity blob runs three times as long as its siblings and sets the floor for the whole module.
/// Those are shapes, and a shape wants a picture: the output opens in Perfetto or chrome://tracing
/// as it stands.
///
/// Spans measure execution only, not the wait for a parallelism slot. A span that looks idle in
/// the viewer really was idle, rather than queued behind the concurrency limit, which is what
/// makes gaps in the picture worth investigating.
/// </remarks>
public sealed class BuildTimeline
{
    private readonly Stopwatch Clock = Stopwatch.StartNew();

    private readonly ConcurrentBag<TimelineSpan> Spans = new();

    /// <summary>Offset from the start of the build, for stamping a span.</summary>
    public TimeSpan Now => Clock.Elapsed;

    public void Record(string Name, string Category, TimeSpan Start, TimeSpan Duration, bool bSucceeded)
    {
        Spans.Add(new TimelineSpan(Name, Category, Start, Duration, bSucceeded));
    }

    /// <summary>
    /// Writes the Chrome Trace Event format, which Perfetto and chrome://tracing both read.
    /// </summary>
    /// <remarks>
    /// Actions are assigned to lanes so that overlapping ones stack instead of drawing over each
    /// other. The lane count that falls out is the parallelism the build actually achieved, which
    /// is the number worth comparing against the one it was allowed.
    /// </remarks>
    public void Write(string FilePath)
    {
        List<TimelineSpan> Ordered = Spans.OrderBy(S => S.Start).ToList();
        List<TimeSpan> LaneEnds = new();

        PathUtils.EnsureDirectoryForFile(FilePath);

        using FileStream Stream = File.Create(FilePath);
        using Utf8JsonWriter Writer = new(Stream, new JsonWriterOptions { Indented = false });

        Writer.WriteStartObject();
        Writer.WriteStartArray("traceEvents");

        foreach (TimelineSpan Span in Ordered)
        {
            int Lane = LaneEnds.FindIndex(End => End <= Span.Start);

            if (Lane < 0)
            {
                Lane = LaneEnds.Count;
                LaneEnds.Add(TimeSpan.Zero);
            }

            LaneEnds[Lane] = Span.Start + Span.Duration;

            Writer.WriteStartObject();
            Writer.WriteString("name", Span.Name);
            Writer.WriteString("cat", Span.bSucceeded ? Span.Category : Span.Category + ",failed");
            Writer.WriteString("ph", "X");
            Writer.WriteNumber("ts", (long)(Span.Start.TotalMilliseconds * 1000.0));
            Writer.WriteNumber("dur", Math.Max(1L, (long)(Span.Duration.TotalMilliseconds * 1000.0)));
            Writer.WriteNumber("pid", 1);
            Writer.WriteNumber("tid", Lane);
            Writer.WriteEndObject();
        }

        Writer.WriteEndArray();
        Writer.WriteEndObject();
        Writer.Flush();

        Log.Info(
            "Build timeline: {0} ({1} actions across {2} lanes)",
            FilePath,
            Ordered.Count,
            Math.Max(LaneEnds.Count, 1));
    }

    /// <summary>
    /// Logs the longest actions, which is the part of the picture worth having without opening a
    /// viewer: the tail of a parallel build is what sets its wall time.
    /// </summary>
    public void LogSlowest(int Count)
    {
        List<TimelineSpan> Slowest = Spans
            .OrderByDescending(S => S.Duration)
            .Take(Count)
            .ToList();

        if (Slowest.Count == 0)
        {
            return;
        }

        double TotalWork = Spans.Sum(S => S.Duration.TotalSeconds);

        Log.Info("Slowest actions ({0:F1}s of work total):", TotalWork);

        foreach (TimelineSpan Span in Slowest)
        {
            Log.Info("  {0,7:F2}s  {1}", Span.Duration.TotalSeconds, Span.Name);
        }
    }
}
