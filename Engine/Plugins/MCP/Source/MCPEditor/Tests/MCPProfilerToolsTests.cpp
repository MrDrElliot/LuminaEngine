#include <gtest/gtest.h>

#include "Containers/StringFormat.h"
#include "MCPProfilerTools.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPProfilerToolsTestsInternal
{
    constexpr const char* CpuHeader = "name;src_file;src_line;total_ns;total_perc;counts;mean_ns;min_ns;max_ns;std_ns";
    constexpr const char* GpuHeader = "name;src_file;src_line;duration_ns";
    constexpr const char* EventHeader = "name;src_file;src_line;ns_since_start;exec_time_ns;thread";
    constexpr const char* FrameZone = "Lumina::FEngine::Update";

    FString EventRow(const char* Name, int64 StartUs, int64 DurationUs, int32 Thread = 1)
    {
        return Lumina::Format("{};z.cpp;1;{};{};{}", Name, StartUs * 1000, DurationUs * 1000, Thread);
    }

    // Frames that start the given spans apart, each followed by the next, plus the last frame that closes them.
    TVector<FString> FrameRows(std::initializer_list<int64> SpansMs)
    {
        TVector<FString> Rows;
        int64 StartMs = 0;
        for (int64 SpanMs : SpansMs)
        {
            Rows.Add(EventRow(FrameZone, StartMs * 1000, 1000));
            StartMs += SpanMs;
        }
        Rows.Add(EventRow(FrameZone, StartMs * 1000, 1000));
        return Rows;
    }

    FProfilerZoneEvents ReadEvents(const TVector<FString>& Rows)
    {
        FProfilerZoneEvents Zones;
        AddTracyEventLine(EventHeader, Zones);
        for (const FString& Row : Rows)
        {
            AddTracyEventLine(FStringView(*Row, Row.Len()), Zones);
        }
        return Zones;
    }

    Agent::FToolResult Analyze(const TVector<FString>& Rows, const SProfilerReportParams& Params, SProfilerHitchResult& Out)
    {
        FProfilerZoneEvents Zones = ReadEvents(Rows);
        return AnalyzeProfilerHitches(Zones, Params, Out);
    }

    SProfilerReportParams Threshold(float Ms)
    {
        SProfilerReportParams Params;
        Params.ThresholdMs = Ms;
        return Params;
    }

    THashMap<FString, FProfilerZoneTotals> ReadLines(bool bGpuRows, std::initializer_list<const char*> Lines)
    {
        THashMap<FString, FProfilerZoneTotals> Totals;
        bool bHeaderPending = true;
        for (const char* Line : Lines)
        {
            AddTracyZoneLine(FStringView(Line), bGpuRows, bHeaderPending, Totals);
        }
        return Totals;
    }

    FProfilerZoneTotals MakeTotals(double TotalNs, double MaxNs, int64 Count)
    {
        FProfilerZoneTotals Totals;
        Totals.TotalNs = TotalNs;
        Totals.MaxNs = MaxNs;
        Totals.Count = Count;
        return Totals;
    }
}

using namespace MCPProfilerToolsTestsInternal;

TEST(MCPProfilerTools, SplitKeepsAnEmptyLastField)
{
    const TVector<FStringView> Fields = SplitProfilerFields("a;b;");
    ASSERT_EQ(Fields.Num(), 3);
    EXPECT_EQ(Fields[0], FStringView("a"));
    EXPECT_EQ(Fields[1], FStringView("b"));
    EXPECT_TRUE(Fields[2].IsEmpty());
}

TEST(MCPProfilerTools, SplitKeepsATrailingFieldWithNoSeparator)
{
    const TVector<FStringView> Fields = SplitProfilerFields("zone;;42");
    ASSERT_EQ(Fields.Num(), 3);
    EXPECT_EQ(Fields[0], FStringView("zone"));
    EXPECT_TRUE(Fields[1].IsEmpty());
    EXPECT_EQ(Fields[2], FStringView("42"));
}

TEST(MCPProfilerTools, CpuRowsSharingANameAreSummed)
{
    const THashMap<FString, FProfilerZoneTotals> Totals = ReadLines(false, {
        CpuHeader,
        "Tick;a.cpp;10;3000;0;3;1000;500;2000;0",
        "Tick;b.cpp;20;5000;0;2;2500;1000;4000;0",
        "Draw;c.cpp;30;700;0;1;700;700;700;0",
    });

    ASSERT_EQ(Totals.Num(), 2);
    const FProfilerZoneTotals& Tick = Totals.FindChecked(FString("Tick"));
    EXPECT_DOUBLE_EQ(Tick.TotalNs, 8000.0);
    EXPECT_EQ(Tick.Count, 5);
    EXPECT_DOUBLE_EQ(Tick.MaxNs, 4000.0);
    EXPECT_DOUBLE_EQ(Totals.FindChecked(FString("Draw")).TotalNs, 700.0);
}

TEST(MCPProfilerTools, GpuRowsEachCountAsOne)
{
    const THashMap<FString, FProfilerZoneTotals> Totals = ReadLines(true, {
        GpuHeader,
        "Shadows;r.cpp;1;1500",
        "Shadows;r.cpp;1;2500",
    });

    ASSERT_EQ(Totals.Num(), 1);
    const FProfilerZoneTotals& Shadows = Totals.FindChecked(FString("Shadows"));
    EXPECT_DOUBLE_EQ(Shadows.TotalNs, 4000.0);
    EXPECT_EQ(Shadows.Count, 2);
    EXPECT_DOUBLE_EQ(Shadows.MaxNs, 2500.0);
}

TEST(MCPProfilerTools, ShortRowsAreSkipped)
{
    EXPECT_TRUE(ReadLines(false, { CpuHeader, "Tick;a.cpp;10;3000;0;3;1000;500" }).IsEmpty());
    EXPECT_TRUE(ReadLines(true, { GpuHeader, "Shadows;r.cpp;1" }).IsEmpty());
}

TEST(MCPProfilerTools, HeaderLineIsIgnored)
{
    EXPECT_TRUE(ReadLines(true, { "Shadows;r.cpp;1;1500" }).IsEmpty());

    const THashMap<FString, FProfilerZoneTotals> Totals = ReadLines(false, { CpuHeader, "Tick;a.cpp;10;3000;0;3;1000;500;2000;0" });
    EXPECT_EQ(Totals.Num(), 1);
    EXPECT_EQ(Totals.Find(FString("name")), nullptr);
}

TEST(MCPProfilerTools, RankDividesByFrameCountAndTreatsZeroAsOne)
{
    THashMap<FString, FProfilerZoneTotals> Zones;
    Zones["Tick"] = MakeTotals(8.0e6, 3.0e6, 4);

    const TVector<SProfilerZone> PerFrame = RankProfilerZones(Zones, 4, 10, "");
    ASSERT_EQ(PerFrame.Num(), 1);
    EXPECT_EQ(PerFrame[0].Name, "Tick");
    EXPECT_EQ(PerFrame[0].Count, 4);
    EXPECT_FLOAT_EQ(PerFrame[0].MsPerFrame, 2.0f);
    EXPECT_FLOAT_EQ(PerFrame[0].MeanUs, 2000.0f);
    EXPECT_FLOAT_EQ(PerFrame[0].MaxUs, 3000.0f);

    const TVector<SProfilerZone> NoFrames = RankProfilerZones(Zones, 0, 10, "");
    ASSERT_EQ(NoFrames.Num(), 1);
    EXPECT_FLOAT_EQ(NoFrames[0].MsPerFrame, 8.0f);
}

TEST(MCPProfilerTools, RankFiltersCaseInsensitively)
{
    THashMap<FString, FProfilerZoneTotals> Zones;
    Zones["RenderShadows"] = MakeTotals(1.0e6, 1.0e6, 1);
    Zones["PhysicsStep"] = MakeTotals(2.0e6, 2.0e6, 1);

    const TVector<SProfilerZone> Ranked = RankProfilerZones(Zones, 1, 10, "shadow");
    ASSERT_EQ(Ranked.Num(), 1);
    EXPECT_EQ(Ranked[0].Name, "RenderShadows");
}

TEST(MCPProfilerTools, RankSortsDescendingAndCapsAtTop)
{
    THashMap<FString, FProfilerZoneTotals> Zones;
    Zones["Small"] = MakeTotals(1.0e6, 1.0e6, 1);
    Zones["Large"] = MakeTotals(3.0e6, 3.0e6, 1);
    Zones["Medium"] = MakeTotals(2.0e6, 2.0e6, 1);
    Zones["Empty"] = MakeTotals(9.0e6, 9.0e6, 0);

    const TVector<SProfilerZone> All = RankProfilerZones(Zones, 1, 10, "");
    ASSERT_EQ(All.Num(), 3);
    EXPECT_EQ(All[0].Name, "Large");
    EXPECT_EQ(All[1].Name, "Medium");
    EXPECT_EQ(All[2].Name, "Small");

    const TVector<SProfilerZone> Capped = RankProfilerZones(Zones, 1, 2, "");
    ASSERT_EQ(Capped.Num(), 2);
    EXPECT_EQ(Capped[0].Name, "Large");
    EXPECT_EQ(Capped[1].Name, "Medium");
}

TEST(MCPProfilerTools, EventHeaderAndShortRowsAreSkipped)
{
    FProfilerZoneEvents Zones;
    AddTracyEventLine("Tick;z.cpp;1;0;1000;1", Zones);
    AddTracyEventLine("Tick;z.cpp;1;0;1000", Zones);
    EXPECT_TRUE(Zones.Events.IsEmpty());

    AddTracyEventLine("Tick;z.cpp;1;5;1000;2", Zones);
    ASSERT_EQ(Zones.Events.Num(), 1);
    EXPECT_EQ(Zones.Names[Zones.Events[0].Name], "Tick");
    EXPECT_EQ(Zones.Events[0].Start, 5);
    EXPECT_EQ(Zones.Events[0].Duration, 1000);
    EXPECT_EQ(Zones.Events[0].Thread, 2);
}

TEST(MCPProfilerTools, HitchSelfTimeSubtractsNestedChildrenOnEachThread)
{
    TVector<FString> Rows = {
        EventRow(FrameZone, 0, 10000),
        EventRow("Outer", 1000, 4000),
        EventRow("Inner", 2000, 1000),
        EventRow(FrameZone, 20000, 1000),
        EventRow("Job", 3000, 5000, 2),
        EventRow("JobStep", 4000, 2000, 2),
        EventRow("LateJob", 25000, 1000, 2),
    };

    SProfilerHitchResult Out;
    ASSERT_FALSE(Analyze(Rows, Threshold(1.0f), Out).bIsError);
    ASSERT_EQ(Out.Hitches.Num(), 1);
    const SProfilerHitch& Hitch = Out.Hitches[0];
    EXPECT_FLOAT_EQ(Hitch.FrameMs, 20.0f);
    EXPECT_FLOAT_EQ(Hitch.AtSeconds, 0.0f);

    ASSERT_EQ(Hitch.GameThread.Num(), 3);
    EXPECT_EQ(Hitch.GameThread[0].Name, FrameZone);
    EXPECT_FLOAT_EQ(Hitch.GameThread[0].SelfMs, 6.0f);
    EXPECT_EQ(Hitch.GameThread[1].Name, "Outer");
    EXPECT_FLOAT_EQ(Hitch.GameThread[1].SelfMs, 3.0f);
    EXPECT_EQ(Hitch.GameThread[2].Name, "Inner");
    EXPECT_FLOAT_EQ(Hitch.GameThread[2].SelfMs, 1.0f);

    ASSERT_EQ(Hitch.Workers.Num(), 2);
    EXPECT_EQ(Hitch.Workers[0].Name, "Job");
    EXPECT_FLOAT_EQ(Hitch.Workers[0].SelfMs, 3.0f);
    EXPECT_EQ(Hitch.Workers[1].Name, "JobStep");
    EXPECT_FLOAT_EQ(Hitch.Workers[1].SelfMs, 2.0f);
}

TEST(MCPProfilerTools, HitchFrameSpansToTheNextFrameStart)
{
    SProfilerHitchResult Out;
    ASSERT_FALSE(Analyze(FrameRows({ 10, 10 }), Threshold(100.0f), Out).bIsError);
    EXPECT_EQ(Out.Frames, 2);
    EXPECT_FLOAT_EQ(Out.MedianFrameMs, 10.0f);
}

TEST(MCPProfilerTools, HitchLeavesOutTheLastFrame)
{
    TVector<FString> Rows = FrameRows({ 5, 5 });
    Rows.Last() = EventRow(FrameZone, 10000, 100000);

    SProfilerHitchResult Out;
    ASSERT_FALSE(Analyze(Rows, Threshold(6.0f), Out).bIsError);
    EXPECT_EQ(Out.Frames, 2);
    EXPECT_FLOAT_EQ(Out.P99FrameMs, 5.0f);
    EXPECT_EQ(Out.HitchCount, 0);

    SProfilerHitchResult Single;
    EXPECT_TRUE(Analyze(FrameRows({}), Threshold(6.0f), Single).bIsError);
}

TEST(MCPProfilerTools, HitchWithoutFrameZonesIsAnError)
{
    SProfilerHitchResult Out;
    EXPECT_TRUE(Analyze({ EventRow("Tick", 0, 1000) }, Threshold(1.0f), Out).bIsError);
}

TEST(MCPProfilerTools, HitchMedianAndP99PickFromSortedFrames)
{
    SProfilerHitchResult Out;
    ASSERT_FALSE(Analyze(FrameRows({ 4, 1, 3, 5, 2 }), Threshold(100.0f), Out).bIsError);
    EXPECT_EQ(Out.Frames, 5);
    EXPECT_FLOAT_EQ(Out.MedianFrameMs, 3.0f);
    EXPECT_FLOAT_EQ(Out.P99FrameMs, 5.0f);
}

TEST(MCPProfilerTools, HitchDefaultThresholdIsTwiceTheMedianAndAtLeastEightOver)
{
    SProfilerHitchResult Short;
    ASSERT_FALSE(Analyze(FrameRows({ 3, 3, 3 }), Threshold(0.0f), Short).bIsError);
    EXPECT_FLOAT_EQ(Short.ThresholdMs, 11.0f);

    SProfilerHitchResult Long;
    ASSERT_FALSE(Analyze(FrameRows({ 20, 20, 20 }), Threshold(0.0f), Long).bIsError);
    EXPECT_FLOAT_EQ(Long.ThresholdMs, 40.0f);
}

TEST(MCPProfilerTools, HitchCountsEveryHitchButBreaksDownOnlyTheWorst)
{
    SProfilerReportParams Params = Threshold(15.0f);
    Params.MaxHitches = 2;

    SProfilerHitchResult Out;
    ASSERT_FALSE(Analyze(FrameRows({ 10, 50, 30, 40, 20 }), Params, Out).bIsError);
    EXPECT_EQ(Out.HitchCount, 4);
    ASSERT_EQ(Out.Hitches.Num(), 2);
    EXPECT_FLOAT_EQ(Out.Hitches[0].FrameMs, 50.0f);
    EXPECT_FLOAT_EQ(Out.Hitches[0].AtSeconds, 0.01f);
    EXPECT_FLOAT_EQ(Out.Hitches[1].FrameMs, 40.0f);
    EXPECT_FLOAT_EQ(Out.Hitches[1].AtSeconds, 0.09f);
}
