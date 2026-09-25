#include <gtest/gtest.h>

#include <cstdio>

#include "Containers/Name.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "Platform/Time/PlatformTime.h"
#include "Assets/AssetTypes/Foliage/GrassType.h"
#include "Scripting/ScriptableTest.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaObjectCreationBench
{
    namespace PlatformTime = Lumina::PlatformTime;
    using Lumina::CObject;
    using Lumina::CScriptableTest;
    using Lumina::CEntityScriptTest;
    using Lumina::CGrassType;
    using Lumina::CObjectRefTest;
    using Lumina::FGuid;
    using Lumina::GObjectArray;
    using Lumina::int32;
    using Lumina::uint64;
    using Lumina::NAME_None;
    using Lumina::OF_Transient;

    volatile uint64 GBenchSink = 0;

    constexpr int32 kIterations = 20'000;
    constexpr int32 kRepeats    = 5;

    // Named from the class itself, so a property added to a fixture cannot leave the row lying.
    const char* Label(const Lumina::CClass* Class)
    {
        static char Text[128];
        std::snprintf(Text, sizeof(Text), "%s, %d reflected propert%s",
            Class->GetName().c_str(), (int32)Class->GetProperties().size(),
            Class->GetProperties().size() == 1 ? "y" : "ies");
        return Text;
    }

    void ReportRow(const char* Name, double Millis)
    {
        std::printf("  %-34s %10.3f %12.1f\n", Name, Millis, (Millis * 1'000'000.0) / kIterations);
    }

    // Objects are freed between repeats or the array grows past its pool and the later runs measure that.
    void DestroyAll(Lumina::TVector<CObject*>& Objects)
    {
        for (CObject* Object : Objects)
        {
            GObjectArray.ConditionalDestroy(Object);
        }
        Objects.clear();
    }

    // Teardown stays outside the timed region, or every repeat but the first measures it too.
    template <typename TMake>
    double MeasureConstruction(TMake&& Make)
    {
        Lumina::TVector<CObject*> Objects;
        Objects.reserve(kIterations);

        double Best = 1e30;

        for (int32 Attempt = 0; Attempt < kRepeats; ++Attempt)
        {
            DestroyAll(Objects);

            const uint64 Start = PlatformTime::Cycles();
            for (int32 Index = 0; Index < kIterations; ++Index)
            {
                CObject* Object = Make();
                GBenchSink += (uint64)(void*)Object;
                Objects.push_back(Object);
            }
            const double Millis = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start);

            Best = Millis < Best ? Millis : Best;
        }

        DestroyAll(Objects);
        return Best;
    }

    TEST(ObjectCreationBench, NewObject)
    {
        std::printf("\nCObject construction, %d objects per run\n", kIterations);
        std::printf("  %-34s %10s %12s\n", "shape", "best (ms)", "ns/object");
        std::printf("  %-34s %10s %12s\n", "----------------------------------", "---------", "------------");

        // Forced up front, so the measured runs do not pay for building it and all take the template path.
        CScriptableTest::StaticClass()->GetDefaultObject();
        CEntityScriptTest::StaticClass()->GetDefaultObject();
        CGrassType::StaticClass()->GetDefaultObject();
        CObjectRefTest::StaticClass()->GetDefaultObject();

        ReportRow(Label(CScriptableTest::StaticClass()), MeasureConstruction([]
        {
            return Lumina::NewObject<CScriptableTest>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        }));

        ReportRow(Label(CEntityScriptTest::StaticClass()), MeasureConstruction([]
        {
            return Lumina::NewObject<CEntityScriptTest>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        }));

        ReportRow(Label(CGrassType::StaticClass()), MeasureConstruction([]
        {
            return Lumina::NewObject<CGrassType>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        }));

        // No reflected properties, so the template pass short-circuits and this is the floor.
        ReportRow(Label(CObjectRefTest::StaticClass()), MeasureConstruction([]
        {
            return Lumina::NewObject<CObjectRefTest>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        }));
    }
}
