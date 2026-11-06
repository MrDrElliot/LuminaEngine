#pragma once

#include "Containers/Name.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Containers/HashTable.h"
#include "Core/Templates/Optional.h"
#include "Core/Object/Class.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/SubclassOf.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "UI/UITypes.h"
#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "World/ECS/Entity.h"
#include "InteropTestLibrary.generated.h"

namespace Lumina
{
    // A row type with a derived one below it, so tests can read a table through a base row struct.
    REFLECT()
    struct RUNTIME_API SInteropTestRow : SDataTableRowBase
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        int32 BaseValue = 0;
    };

    REFLECT()
    struct RUNTIME_API SInteropTestDerivedRow : SInteropTestRow
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        int32 DerivedValue = 0;
    };

    // Holds an FName so it cannot blit, which is what makes it bind as an opaque wrapper.
    REFLECT()
    struct SInteropOpaqueStruct
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FName Label;

        PROPERTY(Hidden)
        float Value = 0.0f;

        // A key whose managed form is not its native bytes, which the map binder used to refuse.
        PROPERTY(Hidden)
        THashMap<FString, float> Weights;

        // The same on the value side.
        PROPERTY(Hidden)
        THashMap<int32, FString> Labels;

        // Both sides plain values, so a benchmark can show the marshalled branch folding away.
        PROPERTY(Hidden)
        THashMap<int32, float> Scores;

        // A payload a Nullable cannot spell, which the optional binder used to refuse.
        PROPERTY(Hidden)
        TOptional<FString> Note;
    };

    // One of each property shape a script edits on a native component, for the interop benchmarks.
    REFLECT(Component)
    struct RUNTIME_API SInteropBenchComponent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        TVector<float> Floats;

        PROPERTY(Hidden)
        TVector<FVector3> Points;

        PROPERTY(Hidden)
        TVector<FString> Names;

        PROPERTY(Hidden)
        FString Label;

        PROPERTY(Hidden)
        FName Tag;

        PROPERTY(Hidden)
        THashMap<int32, float> Scores;

        PROPERTY(Hidden)
        SInteropOpaqueStruct Inner;
    };

    // Gives the interop tests reflected statics whose shapes the Reflector had to bind, nothing more.
    REFLECT()
    class RUNTIME_API CInteropTestLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        // Out gets 0..Count-1, so a test can tell a short read from a wrong one.
        FUNCTION()
        static void MakeRange(int32 Count, TVector<int32>& Out);

        // The entity element, which is the shape every converted buffer export actually uses.
        FUNCTION()
        static void MakeEntityRange(int32 Count, TVector<ECS::FEntity>& Out);

        // A blittable struct element, wider than its managed handle would be.
        FUNCTION()
        static void MakeVectorRange(int32 Count, TVector<FVector3>& Out);

        // An object element, which crosses as a raw pointer and is rebuilt through the wrapper cache.
        FUNCTION()
        static void MakeObjectRange(int32 Count, TVector<TStrongObjectPtr<CInteropTestLibrary>>& Out);

        // Reads a class handle back, so a test can pin that a TSubclassOf argument arrives intact.
        FUNCTION()
        static FName ReadClassName(TSubclassOf<CInteropTestLibrary> Class);

        // Counts calls into MakeRange, so a test can pin how many crossings one binding costs.
        FUNCTION()
        static int32 GetMakeRangeCallCount();

        FUNCTION()
        static void ResetMakeRangeCallCount();

        // Deliberately trivial, so a benchmark measures the crossing rather than the work behind it.
        FUNCTION()
        static int32 BenchNoOp();

        FUNCTION()
        static FVector3 BenchAddVectors(FVector3 A, FVector3 B);

        // A string argument, which costs a UTF-8 encode into stack scratch on the managed side.
        FUNCTION()
        static int32 BenchMeasureString(const FString& Text);

        // A string return, which is the two-pass caller-buffer protocol.
        FUNCTION()
        static const FString& BenchGetString();

        // The one-uint64 handle shape a non-reflectable native pointer crosses in.
        FUNCTION()
        static SUIElement MakeHandle(uint64 Value);

        FUNCTION()
        static uint64 ReadHandle(SUIElement Handle);

        // Returns a name of exactly Length characters, so a test can straddle the managed scratch buffer.
        FUNCTION()
        static FString MakeName(int32 Length);

        // Counts calls into MakeName, so a test can pin how many crossings one string return costs.
        FUNCTION()
        static int32 GetMakeNameCallCount();

        FUNCTION()
        static void ResetMakeNameCallCount();

        // An opaque struct argument, which crosses as the address its managed wrapper views.
        FUNCTION()
        static float ReadOpaqueValue(const SInteropOpaqueStruct& Opaque);
    };
}
