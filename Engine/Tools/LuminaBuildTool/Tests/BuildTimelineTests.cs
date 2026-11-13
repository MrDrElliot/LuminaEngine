using System.Text.Json;
using LuminaBuildTool.Execution;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class BuildTimelineTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("BuildTimelineTests").FullName;

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    private static JsonElement[] ReadEvents(string FilePath)
    {
        using JsonDocument Document = JsonDocument.Parse(File.ReadAllText(FilePath));
        return Document.RootElement.GetProperty("traceEvents").EnumerateArray().Select(E => E.Clone()).ToArray();
    }

    [Fact]
    public void Write_NonOverlappingSpansShareALane()
    {
        BuildTimeline Timeline = new();
        Timeline.Record("First", "Compile", TimeSpan.Zero, TimeSpan.FromSeconds(1), bSucceeded: true);
        Timeline.Record("Second", "Compile", TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1), bSucceeded: true);

        string FilePath = Path.Combine(TempDirectory, "NonOverlapping.json");
        Timeline.Write(FilePath);

        JsonElement[] Events = ReadEvents(FilePath);

        Assert.Equal(2, Events.Length);
        Assert.Equal(0, Events[0].GetProperty("tid").GetInt64());
        Assert.Equal(0, Events[1].GetProperty("tid").GetInt64());
    }

    [Fact]
    public void Write_OverlappingSpansLandOnDifferentLanes()
    {
        BuildTimeline Timeline = new();
        Timeline.Record("First", "Compile", TimeSpan.Zero, TimeSpan.FromSeconds(2), bSucceeded: true);
        Timeline.Record("Second", "Compile", TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1), bSucceeded: true);

        string FilePath = Path.Combine(TempDirectory, "Overlapping.json");
        Timeline.Write(FilePath);

        JsonElement[] Events = ReadEvents(FilePath);

        Assert.Equal(2, Events.Length);
        Assert.Equal(0, Events[0].GetProperty("tid").GetInt64());
        Assert.Equal(1, Events[1].GetProperty("tid").GetInt64());
    }

    [Fact]
    public void Write_ZeroDurationSpanStillGetsDurationOfAtLeastOne()
    {
        BuildTimeline Timeline = new();
        Timeline.Record("Instant", "Compile", TimeSpan.Zero, TimeSpan.Zero, bSucceeded: true);

        string FilePath = Path.Combine(TempDirectory, "ZeroDuration.json");
        Timeline.Write(FilePath);

        JsonElement[] Events = ReadEvents(FilePath);

        Assert.Equal(1, Events[0].GetProperty("dur").GetInt64());
    }

    [Fact]
    public void LogSlowest_OrdersByDurationDescendingAndRespectsCount()
    {
        BuildTimeline Timeline = new();
        Timeline.Record("Short", "Compile", TimeSpan.Zero, TimeSpan.FromSeconds(1), bSucceeded: true);
        Timeline.Record("Long", "Compile", TimeSpan.Zero, TimeSpan.FromSeconds(5), bSucceeded: true);
        Timeline.Record("Middle", "Compile", TimeSpan.Zero, TimeSpan.FromSeconds(3), bSucceeded: true);

        TextWriter PreviousOut = Console.Out;
        StringWriter CapturedOut = new();
        Console.SetOut(CapturedOut);

        try
        {
            Timeline.LogSlowest(2);
        }
        finally
        {
            Console.SetOut(PreviousOut);
        }

        string Output = CapturedOut.ToString();
        int LongIndex = Output.IndexOf("Long", StringComparison.Ordinal);
        int MiddleIndex = Output.IndexOf("Middle", StringComparison.Ordinal);

        Assert.True(LongIndex >= 0 && MiddleIndex >= 0 && LongIndex < MiddleIndex);
        Assert.DoesNotContain("Short", Output);
    }
}
