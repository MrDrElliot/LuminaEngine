#pragma once

#include "Containers/Name.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "World/ECS/Entity.h"
#include "InteropTestLibrary.generated.h"

namespace Lumina
{
    /** Gives the interop tests reflected statics whose shapes the Reflector had to bind, nothing more. */
    REFLECT()
    class RUNTIME_API CInteropTestLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        /** Out gets 0..Count-1, so a test can tell a short read from a wrong one. */
        FUNCTION()
        static void MakeRange(int32 Count, TVector<int32>& Out);

        /** The entity element, which is the shape every converted buffer export actually uses. */
        FUNCTION()
        static void MakeEntityRange(int32 Count, TVector<ECS::FEntity>& Out);

        /** A blittable struct element, wider than its managed handle would be. */
        FUNCTION()
        static void MakeVectorRange(int32 Count, TVector<FVector3>& Out);

        /** Counts calls into MakeRange, so a test can pin how many crossings one binding costs. */
        FUNCTION()
        static int32 GetMakeRangeCallCount();

        FUNCTION()
        static void ResetMakeRangeCallCount();

        /** Deliberately trivial, so a benchmark measures the crossing rather than the work behind it. */
        FUNCTION()
        static int32 BenchNoOp();

        FUNCTION()
        static FVector3 BenchAddVectors(FVector3 A, FVector3 B);

        /** A string argument, which costs a UTF-8 encode into stack scratch on the managed side. */
        FUNCTION()
        static int32 BenchMeasureName(const FName& Name);

        /** A string return, which is the two-pass caller-buffer protocol. */
        FUNCTION()
        static FName BenchGetName();

        /** Returns a name of exactly Length characters, so a test can straddle the managed scratch buffer. */
        FUNCTION()
        static FString MakeName(int32 Length);

        /** Counts calls into MakeName, so a test can pin how many crossings one string return costs. */
        FUNCTION()
        static int32 GetMakeNameCallCount();

        FUNCTION()
        static void ResetMakeNameCallCount();
    };
}
