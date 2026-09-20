#include "RuntimePCH.h"

#include "InteropTestLibrary.h"

namespace Lumina
{
    namespace
    {
        int32 GMakeRangeCalls = 0;
        int32 GMakeNameCalls = 0;
    }

    void CInteropTestLibrary::MakeRange(int32 Count, TVector<int32>& Out)
    {
        ++GMakeRangeCalls;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Out.push_back(Index);
        }
    }

    void CInteropTestLibrary::MakeEntityRange(int32 Count, TVector<ECS::FEntity>& Out)
    {
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Out.push_back(ECS::FEntity::FromPacked((uint32)Index));
        }
    }

    void CInteropTestLibrary::MakeVectorRange(int32 Count, TVector<FVector3>& Out)
    {
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Out.push_back(FVector3((float)Index, (float)Index * 2.0f, (float)Index * 3.0f));
        }
    }

    // Its own class default object, so the element is a real reflected CObject with no world to build.
    void CInteropTestLibrary::MakeObjectRange(int32 Count, TVector<TObjectPtr<CInteropTestLibrary>>& Out)
    {
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Out.emplace_back(StaticClass()->GetDefaultObject<CInteropTestLibrary>());
        }
    }

    FName CInteropTestLibrary::ReadClassName(TSubclassOf<CInteropTestLibrary> Class)
    {
        return Class.IsValid() ? Class.Get()->GetName() : FName();
    }

    int32 CInteropTestLibrary::GetMakeRangeCallCount()
    {
        return GMakeRangeCalls;
    }

    void CInteropTestLibrary::ResetMakeRangeCallCount()
    {
        GMakeRangeCalls = 0;
    }

    int32 CInteropTestLibrary::BenchNoOp()
    {
        return 0;
    }

    FVector3 CInteropTestLibrary::BenchAddVectors(FVector3 A, FVector3 B)
    {
        return A + B;
    }

    int32 CInteropTestLibrary::BenchMeasureName(const FName& Name)
    {
        return (int32)Name.length();
    }

    FName CInteropTestLibrary::BenchGetName()
    {
        static const FName Name("InteropBenchmarkName");
        return Name;
    }

    FUIElement CInteropTestLibrary::MakeHandle(uint64 Value)
    {
        FUIElement Handle;
        Handle.Handle = Value;
        return Handle;
    }

    uint64 CInteropTestLibrary::ReadHandle(FUIElement Handle)
    {
        return Handle.Handle;
    }

    FString CInteropTestLibrary::MakeName(int32 Length)
    {
        ++GMakeNameCalls;

        // Cycled rather than repeated, so a truncated or spliced result cannot read as correct.
        FString Text;
        Text.reserve((size_t)Math::Max(Length, 0));
        for (int32 Index = 0; Index < Length; ++Index)
        {
            Text.push_back((char)('a' + (Index % 26)));
        }
        return Text;
    }

    int32 CInteropTestLibrary::GetMakeNameCallCount()
    {
        return GMakeNameCalls;
    }

    void CInteropTestLibrary::ResetMakeNameCallCount()
    {
        GMakeNameCalls = 0;
    }
}
