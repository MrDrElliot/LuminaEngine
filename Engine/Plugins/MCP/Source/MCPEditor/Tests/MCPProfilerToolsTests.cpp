#include <gtest/gtest.h>

#include "MCPProfilerTools.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPProfilerToolsTestsInternal
{
    constexpr const char* CpuHeader = "name;src_file;src_line;total_ns;total_perc;counts;mean_ns;min_ns;max_ns;std_ns";
    constexpr const char* GpuHeader = "name;src_file;src_line;duration_ns";

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
