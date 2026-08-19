#include "gtest/gtest.h"

#include "Core/Math/Math.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Renderer/MeshData.h"
#include "Renderer/MeshDistanceField.h"
#include "Renderer/MeshQuantization.h"

// Distance field builder correctness. The field is built from BAKED MESHLETS, so these fixtures hand
// GenerateMeshlets' output shape directly (one meshlet per shape) rather than going through the
// importer -- which keeps the tests independent of meshopt and of the import pipeline entirely.

using namespace Lumina;

namespace
{
    // Appends one meshlet covering Positions/Triangles, and the surface that indexes it. Triangles are
    // packed exactly as GenerateMeshlets does: one dword per triangle, three micro-indices in the low
    // bytes, relative to the meshlet's vertex base.
    void AddMeshlet(FMeshResource& Resource, const TVector<FVector3>& Positions, const TVector<FUIntVector3>& Triangles)
    {
        const bool bSkinned = Resource.bSkinnedMesh;

        FMeshlet Meshlet;
        Meshlet.VertexOffset   = (uint32)(bSkinned ? Resource.MeshletData.MeshletSkinnedVertices.size()
                                                   : Resource.MeshletData.MeshletVertices.size());
        Meshlet.TriangleOffset = (uint32)Resource.MeshletData.MeshletTriangles.size();
        Meshlet.VertexCount    = (uint32)Positions.size();
        Meshlet.TriangleCount  = (uint32)Triangles.size();
        Meshlet.LODIndex       = 0;

        // Positions are quantized against the meshlet, so the frame has to be derived before any vertex
        // can be written -- exactly the order GenerateMeshlets uses.
        const FMeshletQuantization Quant = ComputeMeshletQuantization(Positions.data(), (uint32)Positions.size());
        ApplyMeshletQuantization(Meshlet, Quant);

        // Whichever stream the resource declares, so a skinned fixture is genuinely populated rather
        // than passing a test for the trivial reason that its vertex stream was empty.
        for (const FVector3& P : Positions)
        {
            if (bSkinned)
            {
                FMeshletSkinnedVertex V{};
                EncodeMeshletPosition(Quant, P, V);
                Resource.MeshletData.MeshletSkinnedVertices.push_back(V);
            }
            else
            {
                FMeshletVertex V{};
                EncodeMeshletPosition(Quant, P, V);
                Resource.MeshletData.MeshletVertices.push_back(V);
            }
        }

        for (const FUIntVector3& T : Triangles)
        {
            Resource.MeshletData.MeshletTriangles.push_back(T.x | (T.y << 8) | (T.z << 16));
        }

        FGeometrySurface Surface;
        Surface.NumLODs             = 1;
        Surface.LODMeshletOffset[0] = (uint32)Resource.MeshletData.Meshlets.size();
        Surface.LODMeshletCount[0]  = 1;

        Resource.MeshletData.Meshlets.push_back(Meshlet);
        Resource.MeshletData.MeshletSpheres.push_back(FMeshletSphere{});
        Resource.MeshletData.MeshletCones.push_back(FMeshletCone{});
        Resource.GeometrySurfaces.push_back(Surface);
    }

    /** Axis-aligned box centered on the origin, outward-facing (CCW seen from outside). */
    void BuildBox(FMeshResource& Resource, FVector3 HalfExtent)
    {
        const float X = HalfExtent.x;
        const float Y = HalfExtent.y;
        const float Z = HalfExtent.z;

        const TVector<FVector3> P = {
            FVector3(-X, -Y, -Z), FVector3( X, -Y, -Z), FVector3( X,  Y, -Z), FVector3(-X,  Y, -Z),
            FVector3(-X, -Y,  Z), FVector3( X, -Y,  Z), FVector3( X,  Y,  Z), FVector3(-X,  Y,  Z),
        };

        const TVector<FUIntVector3> T = {
            { 0, 2, 1 }, { 0, 3, 2 },   // -Z
            { 4, 5, 6 }, { 4, 6, 7 },   // +Z
            { 0, 1, 5 }, { 0, 5, 4 },   // -Y
            { 3, 7, 6 }, { 3, 6, 2 },   // +Y
            { 0, 4, 7 }, { 0, 7, 3 },   // -X
            { 1, 2, 6 }, { 1, 6, 5 },   // +X
        };

        AddMeshlet(Resource, P, T);
    }

    /** Trilinear read of the built volume at a local-space point, decoded back to local units. Mirrors
     *  SampleDistanceFieldLocal in Includes/DistanceField.slang, including the texel-center convention,
     *  so a disagreement between the two shows up here rather than on screen. */
    float SampleVolume(const FDistanceFieldVolume& Volume, FVector3 LocalPosition)
    {
        const FVector3 UVW = (LocalPosition - Volume.VolumeMin) / Volume.VolumeSize;
        const FVector3 Texel(
            UVW.x * Volume.Dimensions.x - 0.5f,
            UVW.y * Volume.Dimensions.y - 0.5f,
            UVW.z * Volume.Dimensions.z - 0.5f);

        auto Fetch = [&](int32 X, int32 Y, int32 Z) -> float
        {
            X = Math::Clamp(X, 0, (int32)Volume.Dimensions.x - 1);
            Y = Math::Clamp(Y, 0, (int32)Volume.Dimensions.y - 1);
            Z = Math::Clamp(Z, 0, (int32)Volume.Dimensions.z - 1);

            const size_t Index = ((size_t)Z * Volume.Dimensions.y + Y) * Volume.Dimensions.x + X;
            const float Encoded = Volume.Distances[Index] / 255.0f;
            return Volume.bTwoSided
                ? Encoded * Volume.MaxDistance
                : (Encoded * 2.0f - 1.0f) * Volume.MaxDistance;
        };

        const int32 X0 = (int32)Math::Floor(Texel.x);
        const int32 Y0 = (int32)Math::Floor(Texel.y);
        const int32 Z0 = (int32)Math::Floor(Texel.z);
        const float Fx = Texel.x - (float)X0;
        const float Fy = Texel.y - (float)Y0;
        const float Fz = Texel.z - (float)Z0;

        auto Lerp = [](float A, float B, float T) { return A + (B - A) * T; };

        const float C00 = Lerp(Fetch(X0, Y0, Z0),     Fetch(X0 + 1, Y0, Z0),     Fx);
        const float C10 = Lerp(Fetch(X0, Y0 + 1, Z0), Fetch(X0 + 1, Y0 + 1, Z0), Fx);
        const float C01 = Lerp(Fetch(X0, Y0, Z0 + 1),     Fetch(X0 + 1, Y0, Z0 + 1),     Fx);
        const float C11 = Lerp(Fetch(X0, Y0 + 1, Z0 + 1), Fetch(X0 + 1, Y0 + 1, Z0 + 1), Fx);

        return Lerp(Lerp(C00, C10, Fy), Lerp(C01, C11, Fy), Fz);
    }

    /** Exact signed distance to an origin-centered box; the analytic reference (Quilez). */
    float ExactBoxDistance(FVector3 P, FVector3 HalfExtent)
    {
        const FVector3 Q = Math::Abs(P) - HalfExtent;
        const FVector3 Outside = Math::Max(Q, FVector3(0.0f));
        return Math::Length(Outside) + Math::Min(Math::Max(Q.x, Math::Max(Q.y, Q.z)), 0.0f);
    }

    SDistanceFieldBuildSettings MakeSettings(uint32 Resolution = 48)
    {
        SDistanceFieldBuildSettings Settings;
        Settings.bEnabled        = true;
        Settings.Resolution      = Resolution;
        Settings.NarrowBandScale = 0.2f;
        Settings.bTwoSided       = false;
        Settings.SourceLOD       = 0;
        return Settings;
    }
}

TEST(DistanceField, DisabledSettingsProduceNoField)
{
    FMeshResource Resource;
    BuildBox(Resource, FVector3(1.0f));

    SDistanceFieldBuildSettings Settings = MakeSettings();
    Settings.bEnabled = false;

    FDistanceFieldVolume Volume;
    EXPECT_FALSE(DistanceField::Build(Resource, Settings, Volume));
    EXPECT_FALSE(Volume.IsValid());
}

TEST(DistanceField, EmptyMeshProducesNoField)
{
    FMeshResource Resource;

    FDistanceFieldVolume Volume;
    EXPECT_FALSE(DistanceField::Build(Resource, MakeSettings(), Volume));
    EXPECT_FALSE(Volume.IsValid());
}

TEST(DistanceField, SkeletalMeshIsRefused)
{
    // A field baked in mesh-local space cannot follow skinning, so it must be refused outright rather
    // than silently describing the bind pose while the character animates.
    FMeshResource Resource;
    Resource.bSkinnedMesh = true;
    BuildBox(Resource, FVector3(1.0f));

    FDistanceFieldVolume Volume;
    EXPECT_FALSE(DistanceField::Build(Resource, MakeSettings(), Volume));
    EXPECT_FALSE(Volume.IsValid());
}

TEST(DistanceField, VolumeCoversPaddedBoundsWithCubicVoxels)
{
    FMeshResource Resource;
    BuildBox(Resource, FVector3(1.0f, 1.0f, 1.0f));

    FDistanceFieldVolume Volume;
    ASSERT_TRUE(DistanceField::Build(Resource, MakeSettings(32), Volume));
    ASSERT_TRUE(Volume.IsValid());

    // Longest mesh extent is 2, band scale 0.2 -> MaxDistance 0.4, so the volume spans 2 + 2*0.4 = 2.8
    // on every axis. Cubic voxels means the derived per-axis step must agree across all three.
    EXPECT_NEAR(Volume.MaxDistance, 0.4f, 1e-5f);

    const FVector3 Step(
        Volume.VolumeSize.x / (float)Volume.Dimensions.x,
        Volume.VolumeSize.y / (float)Volume.Dimensions.y,
        Volume.VolumeSize.z / (float)Volume.Dimensions.z);
    EXPECT_NEAR(Step.x, Step.y, 1e-5f);
    EXPECT_NEAR(Step.x, Step.z, 1e-5f);

    // And it must actually contain the padded bounds, or the outer voxels clamp before reaching the
    // full band and the gradient at the boundary goes flat.
    EXPECT_LE(Volume.VolumeMin.x, -1.4f + 1e-4f);
    EXPECT_GE(Volume.VolumeMin.x + Volume.VolumeSize.x, 1.4f - 1e-4f);
}

TEST(DistanceField, SignIsNegativeInsideAndPositiveOutside)
{
    FMeshResource Resource;
    BuildBox(Resource, FVector3(1.0f, 1.0f, 1.0f));

    FDistanceFieldVolume Volume;
    ASSERT_TRUE(DistanceField::Build(Resource, MakeSettings(48), Volume));

    // Deep inside: the band saturates, so only the SIGN is meaningful here, which is exactly the
    // property the ray vote exists to get right.
    EXPECT_LT(SampleVolume(Volume, FVector3(0.0f, 0.0f, 0.0f)), 0.0f);
    EXPECT_LT(SampleVolume(Volume, FVector3(0.5f, -0.3f, 0.2f)), 0.0f);

    // Just outside each face.
    EXPECT_GT(SampleVolume(Volume, FVector3(1.2f, 0.0f, 0.0f)), 0.0f);
    EXPECT_GT(SampleVolume(Volume, FVector3(0.0f, -1.2f, 0.0f)), 0.0f);
    EXPECT_GT(SampleVolume(Volume, FVector3(0.0f, 0.0f, 1.2f)), 0.0f);
}

TEST(DistanceField, MatchesAnalyticBoxDistanceInsideTheBand)
{
    const FVector3 HalfExtent(1.0f, 1.0f, 1.0f);

    FMeshResource Resource;
    BuildBox(Resource, HalfExtent);

    FDistanceFieldVolume Volume;
    ASSERT_TRUE(DistanceField::Build(Resource, MakeSettings(64), Volume));

    // Points spread across the accurate band on both sides of the surface.
    const TVector<FVector3> Samples = {
        FVector3(1.1f, 0.0f, 0.0f),   FVector3(1.25f, 0.0f, 0.0f),
        FVector3(0.0f, 1.15f, 0.0f),  FVector3(0.0f, 0.0f, -1.2f),
        FVector3(0.9f, 0.0f, 0.0f),   FVector3(0.0f, -0.85f, 0.0f),
        FVector3(1.1f, 1.1f, 0.0f),   FVector3(1.05f, 1.05f, 1.05f),
    };

    // Tolerance is one voxel: a 64^3 grid over a 2.8-unit span puts a voxel at ~0.044, and both the
    // trilinear reconstruction and the 8-bit quantization are bounded well inside that.
    const float VoxelSize = Volume.VolumeSize.x / (float)Volume.Dimensions.x;

    for (const FVector3& P : Samples)
    {
        const float Expected = ExactBoxDistance(P, HalfExtent);
        const float Actual   = SampleVolume(Volume, P);
        EXPECT_NEAR(Actual, Expected, VoxelSize) << "at (" << P.x << ", " << P.y << ", " << P.z << ")";
    }
}

TEST(DistanceField, TwoSidedFieldIsUnsigned)
{
    FMeshResource Resource;
    BuildBox(Resource, FVector3(1.0f));

    SDistanceFieldBuildSettings Settings = MakeSettings(32);
    Settings.bTwoSided = true;

    FDistanceFieldVolume Volume;
    ASSERT_TRUE(DistanceField::Build(Resource, Settings, Volume));
    EXPECT_TRUE(Volume.bTwoSided);

    // The two-sided encoding spends the whole byte on [0, MaxDistance], so the interior -- which a
    // SIGNED field would put at 0.0 (fully negative) -- must instead saturate to the far end.
    const float Interior = SampleVolume(Volume, FVector3(0.0f, 0.0f, 0.0f));
    EXPECT_NEAR(Interior, Volume.MaxDistance, Volume.MaxDistance * 0.02f);
    EXPECT_GT(SampleVolume(Volume, FVector3(1.2f, 0.0f, 0.0f)), 0.0f);

    // And a point near the surface must still read near zero from both sides, which is the property the
    // unsigned encoding exists to preserve.
    const float VoxelSize = Volume.VolumeSize.x / (float)Volume.Dimensions.x;
    EXPECT_NEAR(SampleVolume(Volume, FVector3(1.0f, 0.0f, 0.0f)), 0.0f, VoxelSize);
}

TEST(DistanceField, NonUniformBoundsKeepVoxelsCubic)
{
    // A long thin slab: the axis-count derivation is where a bug would show up as anisotropic voxels,
    // which silently break every central-difference gradient in the shader.
    FMeshResource Resource;
    BuildBox(Resource, FVector3(4.0f, 0.5f, 0.25f));

    FDistanceFieldVolume Volume;
    ASSERT_TRUE(DistanceField::Build(Resource, MakeSettings(48), Volume));

    const float StepX = Volume.VolumeSize.x / (float)Volume.Dimensions.x;
    const float StepY = Volume.VolumeSize.y / (float)Volume.Dimensions.y;
    const float StepZ = Volume.VolumeSize.z / (float)Volume.Dimensions.z;

    EXPECT_NEAR(StepX, StepY, 1e-4f);
    EXPECT_NEAR(StepX, StepZ, 1e-4f);
    EXPECT_LE(Volume.Dimensions.x, 48u);
    EXPECT_GE(Volume.Dimensions.x, Volume.Dimensions.y);
}

TEST(DistanceField, SerializationRoundTrips)
{
    FMeshResource Resource;
    BuildBox(Resource, FVector3(1.0f, 2.0f, 0.5f));

    FDistanceFieldVolume Volume;
    ASSERT_TRUE(DistanceField::Build(Resource, MakeSettings(16), Volume));

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        Writer << Volume;
    }

    FDistanceFieldVolume Restored;
    FMemoryReader Reader(Bytes);
    Reader << Restored;

    EXPECT_EQ(Restored.Dimensions.x, Volume.Dimensions.x);
    EXPECT_EQ(Restored.Dimensions.y, Volume.Dimensions.y);
    EXPECT_EQ(Restored.Dimensions.z, Volume.Dimensions.z);
    EXPECT_NEAR(Restored.MaxDistance, Volume.MaxDistance, 1e-6f);
    EXPECT_NEAR(Restored.VolumeMin.x, Volume.VolumeMin.x, 1e-6f);
    EXPECT_NEAR(Restored.VolumeSize.z, Volume.VolumeSize.z, 1e-6f);
    EXPECT_EQ(Restored.bTwoSided, Volume.bTwoSided);
    EXPECT_EQ(Restored.Distances, Volume.Distances);
    EXPECT_TRUE(Restored.IsValid());
}

// The GPU mirror in Includes/Common.slang is hand-written, so nothing but this catches a member added
// on one side only. Slang reads the header through a pointer under scalar layout, where the total is
// exactly the sum of the declared members.
TEST(DistanceField, MeshletHeaderMatchesGPUMirrorSize)
{
    EXPECT_EQ(sizeof(FMeshletHeaderGPU), 80u);
    EXPECT_EQ(offsetof(FMeshletHeaderGPU, ConesAddress), 32u);
    EXPECT_EQ(offsetof(FMeshletHeaderGPU, DistanceFieldIndex), 40u);
    EXPECT_EQ(offsetof(FMeshletHeaderGPU, DistanceFieldFlags), 44u);
    EXPECT_EQ(offsetof(FMeshletHeaderGPU, DistanceFieldMinX), 48u);
    EXPECT_EQ(offsetof(FMeshletHeaderGPU, DistanceFieldSizeX), 60u);
    EXPECT_EQ(offsetof(FMeshletHeaderGPU, DistanceFieldMaxDistance), 72u);
}
