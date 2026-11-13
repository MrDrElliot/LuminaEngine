#include "gtest/gtest.h"

// Pulls in editor-only headers, so these tests compile out when Tests is built without the editor.
#if WITH_EDITOR

#include "Containers/HashTable.h"
#include "Core/Console/ConsoleVariable.h"
#include "TaskSystem/Scheduler/JobProfiler.h"
#include "TaskSystem/TaskSystem.h"

#include <cstdio>

using namespace Lumina;

namespace
{
    constexpr uint32 kNestingDepth = 64;
}

TEST(JobProfiler, DeepNestingPastMaxDepthDoesNotReadGarbageNames)
{
    FConsoleRegistry::Get().SetAs<bool>("task.Profiler.Enabled", true);

    FJobProfiler& Profiler = FJobProfiler::Get();
    Profiler.BeginFrame();
    ASSERT_TRUE(Profiler.IsEnabled());

    const uint32 Slot = Jobs::GetThreadSlot();

    char Buffers[kNestingDepth][16];
    THashSet<const char*> KnownNames;
    for (uint32 Index = 0; Index < kNestingDepth; ++Index)
    {
        std::snprintf(Buffers[Index], sizeof(Buffers[Index]), "job%u", Index);
        KnownNames.Add(Buffers[Index]);
    }

    for (uint32 Index = 0; Index < kNestingDepth; ++Index)
    {
        Profiler.JobBegin(Slot, Buffers[Index], false);
    }
    for (uint32 Index = 0; Index < kNestingDepth; ++Index)
    {
        Profiler.JobEnd(Slot);
    }

    Profiler.EndFrame();

    uint32 SlotSpans = 0;
    for (const FJobProfSpan& Span : Profiler.GetLatest().Spans)
    {
        if (Span.Worker != Slot || Span.Kind != EJobSpanKind::Ran)
        {
            continue;
        }
        ++SlotSpans;
        EXPECT_TRUE(KnownNames.Contains(Span.Name)) << "span carried a name pointer not written by this test, consistent with an out-of-bounds read";
    }
    EXPECT_GE(SlotSpans, 2 * kNestingDepth - 1);

    // Disable through BeginFrame rather than Shutdown, which frees lanes that running workers may still touch.
    FConsoleRegistry::Get().SetAs<bool>("task.Profiler.Enabled", false);
    Profiler.BeginFrame();
}

#endif
