#include "gtest/gtest.h"

#include "Core/Math/Math.h"
#include "Renderer/MeshData.h"
#include "Renderer/MeshQuantization.h"

// Meshlet position quantization (DXR2 COMPRESSED1). These pin the two properties the renderer depends
// on and cannot check for itself:
//
//   1. Round-trip error stays inside half a quantum. A meshlet is a ~124-triangle patch, so the quantum
//      is tiny relative to the mesh -- but only if the exponent is chosen from BOTH the extent and the
//      anchor range. Sizing it from extent alone passes for a mesh centered on its origin and silently
//      teleports one that is not, which is why FarFromOrigin exists.
//
//   2. Decode is a pure function of (meshlet, vertex) with no accumulated state, so independent passes
//      reconstruct the same position to the BIT. The VisBuffer stores a triangle ID and DeferredMaterial
//      re-derives the vertices from it; a decode that drifted between them would corrupt barycentrics
//      rather than fail visibly. The Slang twin in Includes/Common.slang is written to match this one
//      operation for operation -- if you change either, change both.

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

    // A meshlet-sized patch: 64 vertices spread over a box of the given extent, centered on Origin.
    TVector<FVector3> MakePatch(FVector3 Origin, float Extent)
    {
        TVector<FVector3> Out;
        Out.reserve(MESHLET_MAX_VERTICES);

        for (uint32 i = 0; i < MESHLET_MAX_VERTICES; ++i)
        {
            // Deterministic spread, no RNG: a failure has to be reproducible from the test name alone.
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

// The case that motivates taking the max of the two exponent bounds. A small patch a long way from the
// model origin wants a scale so fine that floor(Min / scale) overflows the signed 24-bit anchor; the
// anchor wraps and the meshlet lands somewhere else entirely. Error here should stay small in ABSOLUTE
// terms, not merely relative -- a wrapped anchor produces error on the order of the position itself.
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
    // Every vertex coincident: zero extent, so the exponent falls back to the anchor bound alone. The
    // encode must not divide by a zero-derived scale or produce NaN.
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

// Anchors are stored sign-extended in 24 bits and the exponent shares PackedAnchorX's high byte, so a
// negative anchor next to a negative exponent is the combination most likely to smear one field into
// the other.
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

// The property the VisBuffer leans on: decode reads only the meshlet and the vertex, so two callers
// that never share state still land on the same bits. Anchor + offset stays under 2^24, which makes the
// int-to-float conversion exact, and the scale is an exact power of two, so the multiply is exact too.
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
    // Guarded by static_assert at their declarations too; restated here so a stride change surfaces as
    // a named failing test rather than only as a compile error in a header most people never open.
    //
    // Keep these in step with the static_asserts in Vertex.h AND with the Slang mirrors in
    // Common.slang -- the numbers below are the ones the GPU reads, and a C++-only change that keeps
    // the static_assert happy by editing it is exactly the stride bug this is here to catch.
    // 28/36 since TEXCOORD_1 added a packed uint UV1 to both.
    EXPECT_EQ(sizeof(FMeshlet), 32u);
    EXPECT_EQ(sizeof(FMeshletVertex), 28u);
    EXPECT_EQ(sizeof(FMeshletSkinnedVertex), 36u);
}
