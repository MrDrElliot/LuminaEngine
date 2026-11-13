using LuminaBuildTool.Modes;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class ProfileModeTests
{
    private const string CpuHeader = "name;src_file;src_line;total_ns;total_perc;counts;mean_ns;min_ns;max_ns;std_ns";
    private const string GpuHeader = "name;src_file;Time from start of program;GPU execution time";

    private static ProfileEntry Entry(string Name, double Self, long Count = 1, double Mean = 0.0)
    {
        return new ProfileEntry(Name, string.Empty, Self, Count, Mean);
    }

    private static string Csv(string Header, params string[] Rows)
    {
        return string.Join('\n', Rows.Prepend(Header)) + "\n";
    }

    [Fact]
    public void ParseReadsColumnsByNameAndSortsBySelfTime()
    {
        List<ProfileEntry> Entries = ProfileMode.Parse(Csv(CpuHeader,
            "Tick;C:/Src/Engine.cpp;42;5000;10;5;1000;900;1100;50",
            "Draw;C:/Src/Renderer.cpp;7;8000;20;4;2000;1900;2100;60\r"));

        Assert.Equal(2, Entries.Count);
        Assert.Equal(new ProfileEntry("Draw", "Renderer.cpp:7", 8000, 4, 2000), Entries[0]);
        Assert.Equal(new ProfileEntry("Tick", "Engine.cpp:42", 5000, 5, 1000), Entries[1]);
    }

    [Fact]
    public void ParseDefaultsMissingOptionalColumnsAndSkipsRowsWithoutTotal()
    {
        List<ProfileEntry> Entries = ProfileMode.Parse(Csv(CpuHeader,
            "Short;C:/Src/Short.cpp;3;3000",
            "NoTotal;C:/Src/NoTotal.cpp;9"));

        Assert.Equal(new[] { new ProfileEntry("Short", "Short.cpp:3", 3000, 0, 0.0) }, Entries);
    }

    [Fact]
    public void ParseLeavesSourceEmptyWithoutSourceColumns()
    {
        List<ProfileEntry> Entries = ProfileMode.Parse(Csv("name;total_ns;counts;mean_ns", "Tick;500;2;250"));

        Assert.Equal(new[] { new ProfileEntry("Tick", string.Empty, 500, 2, 250) }, Entries);
    }

    [Fact]
    public void ParseGpuZonesSubtractsChildTimeFromSelfButNotInclusive()
    {
        List<ProfileEntry> Entries = ProfileMode.ParseGpuZones(Csv(GpuHeader,
            "Nested;C:/Shaders/Nested.slang;15;10",
            "Second;C:/Shaders/Second.slang;50;20",
            "First;C:/Shaders/First.slang;10;30",
            "Frame;C:/Shaders/Frame.slang;0;100"));

        Dictionary<string, ProfileEntry> ByName = Entries.ToDictionary(E => E.Name);

        Assert.Equal(50, ByName["Frame"].SelfNanoseconds);
        Assert.Equal(100, ByName["Frame"].InclusiveNanoseconds);
        Assert.Equal(20, ByName["First"].SelfNanoseconds);
        Assert.Equal(30, ByName["First"].InclusiveNanoseconds);
        Assert.Equal(10, ByName["Nested"].SelfNanoseconds);
        Assert.Equal(20, ByName["Second"].SelfNanoseconds);
        Assert.Equal("Frame.slang", ByName["Frame"].Source);
        Assert.Equal("Frame", Entries[0].Name);
    }

    [Fact]
    public void ParseGpuZonesNestsTheShorterOfTwoZonesStartingTogether()
    {
        List<ProfileEntry> Entries = ProfileMode.ParseGpuZones(Csv(GpuHeader,
            "Child;;200;20",
            "Parent;;200;50",
            "Empty;;300;0",
            "Host;;300;10"));

        Dictionary<string, ProfileEntry> ByName = Entries.ToDictionary(E => E.Name);

        Assert.Equal(30, ByName["Parent"].SelfNanoseconds);
        Assert.Equal(50, ByName["Parent"].InclusiveNanoseconds);
        Assert.Equal(20, ByName["Child"].SelfNanoseconds);
        Assert.Equal(10, ByName["Host"].SelfNanoseconds);
        Assert.Equal(0, ByName["Empty"].SelfNanoseconds);
    }

    [Fact]
    public void ParseGpuZonesSumsRepeatedZonesAndAveragesInclusiveTime()
    {
        List<ProfileEntry> Entries = ProfileMode.ParseGpuZones(Csv(GpuHeader,
            "Pass;;0;10",
            "Pass;;20;30",
            "Inner;;25;5"));

        ProfileEntry Pass = Entries.Single(E => E.Name == "Pass");

        Assert.Equal(2, Pass.Count);
        Assert.Equal(35, Pass.SelfNanoseconds);
        Assert.Equal(40, Pass.InclusiveNanoseconds);
        Assert.Equal(20, Pass.MeanNanoseconds);
    }

    [Fact]
    public void MergeDropsZonesMissingFromARunAndTakesTheMedian()
    {
        List<ProfileEntry> Merged = ProfileMode.Merge(new List<List<ProfileEntry>>
        {
            new() { Entry("Tick", 120, 4, 30), Entry("Flaky", 500) },
            new() { Entry("Tick", 100, 6, 10), Entry("Draw", 50, 2, 25) },
            new() { Entry("Tick", 110, 5, 20), Entry("Draw", 50, 2, 25), Entry("Flaky", 400) },
        });

        Assert.DoesNotContain(Merged, E => E.Name == "Flaky");
        Assert.DoesNotContain(Merged, E => E.Name == "Draw");

        ProfileEntry Tick = Assert.Single(Merged);
        Assert.Equal(110, Tick.SelfNanoseconds);
        Assert.Equal(5, Tick.Count);
        Assert.Equal(20, Tick.MeanNanoseconds);
        Assert.Equal(20.0, Tick.SpreadPercent, 9);
    }

    [Fact]
    public void MergeReportsSpreadPerKeptZone()
    {
        List<ProfileEntry> Merged = ProfileMode.Merge(new List<List<ProfileEntry>>
        {
            new() { Entry("Tick", 100), Entry("Idle", 0) },
            new() { Entry("Tick", 150), Entry("Idle", 0) },
        });

        Assert.Equal(new[] { "Tick", "Idle" }, Merged.Select(E => E.Name));
        Assert.Equal(125, Merged[0].SelfNanoseconds);
        Assert.Equal(50.0, Merged[0].SpreadPercent, 9);
        Assert.Equal(0.0, Merged[1].SpreadPercent);
    }

    [Fact]
    public void MergeReturnsASingleRunUnchanged()
    {
        List<ProfileEntry> Run = new() { Entry("Tick", 100) };

        Assert.Same(Run, ProfileMode.Merge(new List<List<ProfileEntry>> { Run }));
    }

    [Theory]
    [InlineData(new double[] { }, 0.0)]
    [InlineData(new double[] { 7 }, 7.0)]
    [InlineData(new double[] { 1, 2, 9 }, 2.0)]
    [InlineData(new double[] { 1, 2, 4, 9 }, 3.0)]
    public void MedianHandlesOddAndEvenLengths(double[] Sorted, double Expected)
    {
        Assert.Equal(Expected, ProfileMode.Median(Sorted));
    }
}
