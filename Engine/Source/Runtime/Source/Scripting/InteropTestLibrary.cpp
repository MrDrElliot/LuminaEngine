#include "RuntimePCH.h"

#include "InteropTestLibrary.h"
#include "Scripting/DotNet/DotNetExport.h"

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
            Out.Add(Index);
        }
    }

    void CInteropTestLibrary::MakeEntityRange(int32 Count, TVector<ECS::FEntity>& Out)
    {
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Out.Add(ECS::FEntity::FromPacked((uint32)Index));
        }
    }

    void CInteropTestLibrary::MakeVectorRange(int32 Count, TVector<FVector3>& Out)
    {
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Out.Emplace((float)Index, (float)Index * 2.0f, (float)Index * 3.0f);
        }
    }

    // Its own class default object, so the element is a real reflected CObject with no world to build.
    void CInteropTestLibrary::MakeObjectRange(int32 Count, TVector<TStrongObjectPtr<CInteropTestLibrary>>& Out)
    {
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Out.Emplace(StaticClass()->GetDefaultObject<CInteropTestLibrary>());
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

    int32 CInteropTestLibrary::BenchMeasureString(const FString& Text)
    {
        return Text.Len();
    }

    // Returned by reference, so the benchmark prices the crossing and not a native allocation per call.
    const FString& CInteropTestLibrary::BenchGetString()
    {
        static const FString Name("InteropBenchmarkName");
        return Name;
    }

    SUIElement CInteropTestLibrary::MakeHandle(uint64 Value)
    {
        SUIElement Handle;
        Handle.Handle = Value;
        return Handle;
    }

    uint64 CInteropTestLibrary::ReadHandle(SUIElement Handle)
    {
        return Handle.Handle;
    }

    FString CInteropTestLibrary::MakeName(int32 Length)
    {
        ++GMakeNameCalls;

        // Cycled rather than repeated, so a truncated or spliced result cannot read as correct.
        FString Text;
        Text.Reserve(Math::Max(Length, 0));
        for (int32 Index = 0; Index < Length; ++Index)
        {
            Text.AppendChar((char)('a' + (Index % 26)));
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

    float CInteropTestLibrary::ReadOpaqueValue(const SInteropOpaqueStruct& Opaque)
    {
        return Opaque.Value + (float)Opaque.Label.Len();
    }
}

// The same trivial calls for NativeCall and P/Invoke alike, so a benchmark row differs only in how managed code got here.
LUMINA_DOTNET_EXPORT(Lumina::int32, InteropBench_NoOp)()
{
    return 0;
}

LUMINA_DOTNET_EXPORT(Lumina::FVector3, InteropBench_AddVectors)(Lumina::FVector3 A, Lumina::FVector3 B)
{
    return A + B;
}

LUMINA_DOTNET_EXPORT(Lumina::int32, InteropBench_CountedUtf8)(const char* Text, Lumina::int32 Length)
{
    return Length > 0 ? Text[0] : 0;
}

LUMINA_DOTNET_EXPORT(Lumina::int32, InteropBench_TerminatedUtf8)(const char* Text)
{
    return Text != nullptr ? Text[0] : 0;
}

LUMINA_DOTNET_EXPORT(Lumina::int32, InteropBench_TerminatedUtf16)(const char16_t* Text)
{
    return Text != nullptr ? Text[0] : 0;
}

LUMINA_DOTNET_EXPORT(const char*, InteropBench_StaticUtf8)()
{
    return "InteropBenchmarkName";
}
