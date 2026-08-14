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

/// <summary>Records when each action ran and for how long, and writes it as a trace a profiler can draw.</summary>
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

    /// <summary>Writes the Chrome Trace Event format, which Perfetto and chrome://tracing both read.</summary>
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

    /// <summary>Logs the longest actions: the tail of a parallel build is what sets its wall time.</summary>
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
