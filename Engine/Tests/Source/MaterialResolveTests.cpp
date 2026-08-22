#include "gtest/gtest.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"

// A recompile that changes bytecode must be observable and one that does not must be free.

namespace Lumina
{
    namespace
    {
        // Arbitrary words, since Commit only hashes and stores the blob.
        TVector<uint32> MakeBlob(uint32 Seed, uint32 Words = 8)
        {
            TVector<uint32> Blob;
            Blob.reserve(Words);
            for (uint32 i = 0; i < Words; ++i)
            {
                Blob.push_back(Seed * 2654435761u + i);
            }
            return Blob;
        }

        // GShaderLibrary is a plain pointer, so a test can stand one up without a device.
        struct FScopedShaderLibrary
        {
            FShaderLibrary Library;
            FShaderLibrary* Previous = nullptr;

            FScopedShaderLibrary()
            {
                Previous = GShaderLibrary;
                GShaderLibrary = &Library;
            }
            ~FScopedShaderLibrary()
            {
                GShaderLibrary = Previous;
            }
        };
    }

    // Two materials compiling to identical bytecode must receive the SAME entry and one draw.
    TEST(ShaderLibrary, IdenticalBytecodeSharesOneEntry)
    {
        FScopedShaderLibrary Scope;

        const TVector<uint32> Blob = MakeBlob(1);

        const FShaderH A = FShaderLibrary::Commit(FName("MatA_PS"), ERHIShaderType::Fragment,
                                                       TSpan<const uint32>(Blob.data(), Blob.size()));
        // Deliberately a DIFFERENT key name with the same content, which is the sharing case.
        const FShaderH B = FShaderLibrary::Commit(FName("MatB_PS"), ERHIShaderType::Fragment,
                                                       TSpan<const uint32>(Blob.data(), Blob.size()));

        ASSERT_NE(A, nullptr);
        EXPECT_EQ(A, B);
    }

    // An unchanged recompile must not move the generation, or every pipeline rebuilds.
    TEST(ShaderLibrary, UnchangedRecommitDoesNotBumpGeneration)
    {
        FScopedShaderLibrary Scope;

        const TVector<uint32> Blob = MakeBlob(2);

        const FShaderH First = FShaderLibrary::Commit(FName("Mat_PS"), ERHIShaderType::Fragment,
                                                          TSpan<const uint32>(Blob.data(), Blob.size()));
        ASSERT_NE(First, nullptr);

        const uint64 HashBefore = First.Handle;

        const FShaderH Second = FShaderLibrary::Commit(FName("Mat_PS"), ERHIShaderType::Fragment,
                                                           TSpan<const uint32>(Blob.data(), Blob.size()));

        EXPECT_EQ(First, Second);
        EXPECT_EQ(HashBefore, Second.Handle);
    }

    // Changed bytecode must change PipelineHash, because that hash IS the pipeline cache key.
    TEST(ShaderLibrary, ChangedBytecodeChangesPipelineHash)
    {
        FScopedShaderLibrary Scope;

        const TVector<uint32> BlobA = MakeBlob(3);
        const TVector<uint32> BlobB = MakeBlob(4);

        const FShaderH A = FShaderLibrary::Commit(FName("Mat_PS"), ERHIShaderType::Fragment,
                                                      TSpan<const uint32>(BlobA.data(), BlobA.size()));
        ASSERT_NE(A, nullptr);
        const uint64 HashA = A.Handle;

        const FShaderH B = FShaderLibrary::Commit(FName("Mat_PS"), ERHIShaderType::Fragment,
                                                      TSpan<const uint32>(BlobB.data(), BlobB.size()));
        ASSERT_NE(B, nullptr);

        EXPECT_NE(HashA, B.Handle);
    }

    // ShaderRevision must move on a real swap and stay put otherwise.
    TEST(MaterialShaderRevision, MovesOnlyWhenAnEntryIsActuallySwapped)
    {
        FScopedShaderLibrary Scope;

        CMaterial* Material = NewObject<CMaterial>();
        ASSERT_NE(Material, nullptr);

        const uint32 Start = Material->GetShaderRevision();

        const TVector<uint32> BlobA = MakeBlob(5);
        Material->CommitShaderStage(EMaterialShaderStage::Pixel,
                                    TSpan<const uint32>(BlobA.data(), BlobA.size()));

        const uint32 AfterFirst = Material->GetShaderRevision();
        EXPECT_NE(Start, AfterFirst) << "first commit installs an entry and must be observable";

        // Same bytecode, so content-keyed Commit hands back the same entry and nothing was superseded.
        Material->CommitShaderStage(EMaterialShaderStage::Pixel,
                                    TSpan<const uint32>(BlobA.data(), BlobA.size()));
        EXPECT_EQ(AfterFirst, Material->GetShaderRevision()) << "an unchanged recompile must not churn";

        // Different bytecode gets a new entry, so anything caching the old pointer is stale.
        const TVector<uint32> BlobB = MakeBlob(6);
        Material->CommitShaderStage(EMaterialShaderStage::Pixel,
                                    TSpan<const uint32>(BlobB.data(), BlobB.size()));
        EXPECT_NE(AfterFirst, Material->GetShaderRevision()) << "a real recompile must be observable";
    }

    // A resolved surface must report itself stale once its source material recompiles.
    TEST(MaterialResolve, SurfaceReportsStaleAfterSourceRecompiles)
    {
        FScopedShaderLibrary Scope;

        CMaterial* Material = NewObject<CMaterial>();
        ASSERT_NE(Material, nullptr);

        const TVector<uint32> BlobA = MakeBlob(7);
        Material->CommitShaderStage(EMaterialShaderStage::Pixel,
                                    TSpan<const uint32>(BlobA.data(), BlobA.size()));

        // The same call ResolveSurfaceMaterial makes, so a hand-rolled key cannot drift.
        FResolvedSurface Surface;
        MeshResolve::StampSurfaceSource(Surface, Material);

        EXPECT_FALSE(MeshResolve::IsSurfaceStale(Surface)) << "fresh resolve must not report stale";

        const TVector<uint32> BlobB = MakeBlob(8);
        Material->CommitShaderStage(EMaterialShaderStage::Pixel,
                                    TSpan<const uint32>(BlobB.data(), BlobB.size()));

        EXPECT_TRUE(MeshResolve::IsSurfaceStale(Surface)) << "recompile must supersede the cached entries";
    }

    // Dropping the last strong reference must free the entry and stale every weak handle.
    TEST(ShaderLibrary, LastReleaseFreesTheEntryAndStalesItsHandles)
    {
        FScopedShaderLibrary Scope;

        const TVector<uint32> Blob = MakeBlob(11);

        const FShaderH Owned = FShaderLibrary::Commit(FName("Free_PS"), ERHIShaderType::Fragment,
                                                      TSpan<const uint32>(Blob.data(), Blob.size()));
        ASSERT_NE(Owned, nullptr);
        ASSERT_NE(FShaderLibrary::Resolve(Owned), nullptr);

        // A cache would hold exactly this, a copy of the handle with no reference taken.
        const FShaderH Weak = Owned;

        FShaderLibrary::Release(Owned);

        // Deferred on purpose, since the free only happens at a frame boundary.
        EXPECT_NE(FShaderLibrary::Resolve(Weak), nullptr) << "release must not free inline";

        FShaderLibrary::FlushPendingReleases();

        EXPECT_EQ(FShaderLibrary::Resolve(Weak), nullptr) << "weak handle must go stale once freed";
    }

    // Two materials share ONE entry, so one releasing must not pull the shaders from the other.
    TEST(ShaderLibrary, SharedEntrySurvivesOneOwnerReleasing)
    {
        FScopedShaderLibrary Scope;

        const TVector<uint32> Blob = MakeBlob(12);

        const FShaderH A = FShaderLibrary::Commit(FName("ShareA_PS"), ERHIShaderType::Fragment,
                                                  TSpan<const uint32>(Blob.data(), Blob.size()));
        const FShaderH B = FShaderLibrary::Commit(FName("ShareB_PS"), ERHIShaderType::Fragment,
                                                  TSpan<const uint32>(Blob.data(), Blob.size()));
        ASSERT_EQ(A, B) << "identical bytecode must intern to one entry";

        FShaderLibrary::Release(A);
        FShaderLibrary::FlushPendingReleases();

        EXPECT_NE(FShaderLibrary::Resolve(B), nullptr) << "the second owner still holds a reference";

        FShaderLibrary::Release(B);
        FShaderLibrary::FlushPendingReleases();

        EXPECT_EQ(FShaderLibrary::Resolve(B), nullptr) << "last owner gone, entry must be freed";
    }

    // A surface that never resolved a material has nothing to go stale against.
    TEST(MaterialResolve, SurfaceWithNoSourceMaterialIsNeverStale)
    {
        FResolvedSurface Surface;
        EXPECT_FALSE(MeshResolve::IsSurfaceStale(Surface));
    }
}
