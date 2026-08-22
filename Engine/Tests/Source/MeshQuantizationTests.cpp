#include "gtest/gtest.h"

#include "Core/Math/Math.h"
#include "Renderer/MeshData.h"
#include "Renderer/MeshQuantization.h"

// Round-trip stays inside half a quantum and decode is bit-reproducible; Common.slang must match.

using namespace Lumina;

namespace
{
    struct FRoundTrip
    {
        float MaxError = 0.0f;
        float Quantum  = 0.0f;
    };

    FRoundTrip RoundTrip(const TVector<FVector3>& Positions)
    {
        const FMeshletQuantization Q = ComputeMeshletQuantization(Positions.data(), (uint32)Positions.size());

        FMeshlet M{};
        M.VertexCount = (uint32)Positions.size();
        ApplyMeshletQuantization(M, Q);

        FRoundTrip Result;
        Result.Quantum = MeshletExponentScale(Q.Exponent);

        for (const FVector3& P : Positions)
        {
            FMeshletVertex V{};
            EncodeMeshletPosition(Q, P, V);

            const FVector3 D = DecodeMeshletPosition(M, V);
            Result.MaxError = Math::Max(Result.MaxError, Math::Abs(D.x - P.x));
            Result.MaxError = Math::Max(Result.MaxError, Math::Abs(D.y - P.y));
            Result.MaxError = Math::Max(Result.MaxError, Math::Abs(D.z - P.z));
        }

        return Result;
    }

    // A meshlet-sized patch of 64 vertices over a box of the given extent, centered on Origin.
    TVector<FVector3> MakePatch(FVector3 Origin, float Extent)
    {
        TVector<FVector3> Out;
        Out.reserve(MESHLET_MAX_VERTICES);

        for (uint32 i = 0; i < MESHLET_MAX_VERTICES; ++i)
        {
            // Deterministic spread with no RNG, so a failure reproduces from the test name alone.
            const float U = (float)(i % 4) / 3.0f;
            const float V = (float)((i / 4) % 4) / 3.0f;
            const float W = (float)((i / 16) % 4) / 3.0f;

            Out.push_back(FVector3(Origin.x + (U - 0.5f) * Extent,
                                   Origin.y + (V - 0.5f) * Extent,
                                   Origin.z + (W - 0.5f) * Extent));
        }
        return Out;
    }
}

TEST(MeshQuantization, RoundTripWithinHalfQuantum)
{
    const FRoundTrip R = RoundTrip(MakePatch(FVector3(0.0f, 0.0f, 0.0f), 2.0f));
    EXPECT_LE(R.MaxError, R.Quantum * 0.5f);
}

// A small patch far from the origin overflows the 24-bit anchor unless both bounds are taken.
TEST(MeshQuantization, FarFromOriginDoesNotWrapAnchor)
{
    const FRoundTrip R = RoundTrip(MakePatch(FVector3(1000.0f, -750.0f, 1200.0f), 0.1f));

    EXPECT_LE(R.MaxError, R.Quantum * 0.5f);
    EXPECT_LT(R.MaxError, 0.01f);
}

TEST(MeshQuantization, LargePatchStillRoundTrips)
{
    const FRoundTrip R = RoundTrip(MakePatch(FVector3(0.0f, 0.0f, 0.0f), 4096.0f));
    EXPECT_LE(R.MaxError, R.Quantum * 0.5f);
}

TEST(MeshQuantization, DegenerateMeshletIsExact)
{
    // Zero extent falls back to the anchor bound, and encode must not divide by zero or NaN.
    TVector<FVector3> Positions(MESHLET_MAX_VERTICES, FVector3(3.5f, -2.25f, 0.0f));

    const FRoundTrip R = RoundTrip(Positions);
    EXPECT_LE(R.MaxError, R.Quantum * 0.5f);
}

TEST(MeshQuantization, EmptyMeshletIsSafe)
{
    const FMeshletQuantization Q = ComputeMeshletQuantization(nullptr, 0);
    EXPECT_EQ(Q.AnchorX, 0);
    EXPECT_EQ(Q.AnchorY, 0);
    EXPECT_EQ(Q.AnchorZ, 0);
    EXPECT_EQ(Q.Exponent, 0);
}

// A negative anchor beside a negative exponent is where one field smears into the other.
TEST(MeshQuantization, NegativeAnchorAndExponentSurvivePacking)
{
    FMeshletQuantization Q;
    Q.AnchorX  = -8388608;   // int24 min
    Q.AnchorY  = -1;
    Q.AnchorZ  =  8388607;   // int24 max
    Q.Exponent = -126;

    FMeshlet M{};
    ApplyMeshletQuantization(M, Q);

    EXPECT_EQ(UnpackMeshletAnchor(M.PackedAnchorX), Q.AnchorX);
    EXPECT_EQ(UnpackMeshletAnchor(M.PackedAnchorY), Q.AnchorY);
    EXPECT_EQ(UnpackMeshletAnchor(M.PackedAnchorZ), Q.AnchorZ);
    EXPECT_EQ(UnpackMeshletExponent(M.PackedAnchorX), Q.Exponent);
}

// Decode reads only the meshlet and vertex, so two callers land on the same bits.
TEST(MeshQuantization, DecodeIsBitwiseReproducible)
{
    const TVector<FVector3> Positions = MakePatch(FVector3(17.0f, -4.0f, 9.5f), 3.0f);
    const FMeshletQuantization Q = ComputeMeshletQuantization(Positions.data(), (uint32)Positions.size());

    FMeshlet M{};
    ApplyMeshletQuantization(M, Q);

    for (const FVector3& P : Positions)
    {
        FMeshletVertex V{};
        EncodeMeshletPosition(Q, P, V);

        const FVector3 A = DecodeMeshletPosition(M, V);
        const FVector3 B = DecodeMeshletPosition(M, V);

        EXPECT_EQ(memcmp(&A, &B, sizeof(FVector3)), 0);
    }
}

TEST(MeshQuantization, StructLayoutsMatchTheGPUMirrors)
{
    // Keep these in step with the static_asserts in Vertex.h and the mirrors in Common.slang.
    EXPECT_EQ(sizeof(FMeshlet), 32u);
    EXPECT_EQ(sizeof(FMeshletVertex), 28u);
    EXPECT_EQ(sizeof(FMeshletSkinnedVertex), 36u);
}
