#include "gtest/gtest.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"

// An editor save drops SPIR-V a graph can rebuild, so a shader template edit never changes the asset.

namespace Lumina
{
    namespace
    {
        TVector<uint32> MakeBlob(uint32 Seed)
        {
            TVector<uint32> Blob;
            for (uint32 i = 0; i < 8; ++i)
            {
                Blob.push_back(Seed * 2246822519u + i);
            }
            return Blob;
        }

        CMaterial* SaveAndLoad(CMaterial* Source)
        {
            TVector<uint8> Bytes;
            {
                FMemoryWriter Writer(Bytes);
                FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
                Source->Serialize(Ar);
            }

            CMaterial* Loaded = NewObject<CMaterial>();
            FMemoryReader Reader(Bytes);
            FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
            Loaded->Serialize(Ar);
            return Loaded;
        }

        class MaterialShaderCache : public ::testing::Test
        {
        protected:
            // Registers the compiled-in classes' properties, without which a class serializes nothing.
            static void SetUpTestSuite() { ProcessNewlyLoadedCObjects(); }
        };
    }

    TEST_F(MaterialShaderCache, AnEditorSaveKeepsOnlySpirvNoGraphCanRebuild)
    {
        CMaterial* Source = NewObject<CMaterial>();
        ASSERT_NE(Source, nullptr);

        const TVector<uint32> FromGraph = MakeBlob(1);
        const TVector<uint32> NoGraph   = MakeBlob(2);
        Source->SetStageBinaries(EMaterialShaderStage::Pixel, TSpan<const uint32>(FromGraph.data(), FromGraph.size()), 0xABCDull);
        Source->SetStageBinaries(EMaterialShaderStage::Vertex, TSpan<const uint32>(NoGraph.data(), NoGraph.size()));
        Source->CompiledTemplateHash = 42;

        CMaterial* Loaded = SaveAndLoad(Source);
        ASSERT_NE(Loaded, nullptr);

        EXPECT_TRUE(Loaded->GetShaderStageBinaries(EMaterialShaderStage::Pixel).empty());
        EXPECT_EQ(Loaded->GetShaderStageBinaries(EMaterialShaderStage::Vertex), NoGraph);
        EXPECT_EQ(Loaded->CompiledTemplateHash, 42ull) << "kept SPIR-V still needs its template stamp";

        // The save works on a copy, so the live material keeps drawing with what it compiled.
        EXPECT_EQ(Source->GetShaderStageBinaries(EMaterialShaderStage::Pixel), FromGraph);
    }

    TEST_F(MaterialShaderCache, ACompiledStageKeepsItsHashThroughThePostLoadRecommit)
    {
        FShaderLibrary  Library;
        FShaderLibrary* Previous = GShaderLibrary;
        GShaderLibrary = &Library;

        CMaterial* Source = NewObject<CMaterial>();
        ASSERT_NE(Source, nullptr);

        const TVector<uint32> FromGraph = MakeBlob(4);
        Source->CommitShaderStage(EMaterialShaderStage::Pixel, TSpan<const uint32>(FromGraph.data(), FromGraph.size()), 0x5678ull);

        const TVector<uint32>& Stored = Source->GetShaderStageBinaries(EMaterialShaderStage::Pixel);
        Source->CommitShaderStage(EMaterialShaderStage::Pixel, TSpan<const uint32>(Stored.data(), Stored.size()), Source->Stages[0].SourceHash);
        EXPECT_EQ(Source->Stages[0].SourceHash, 0x5678ull);

        CMaterial* Loaded = SaveAndLoad(Source);
        EXPECT_TRUE(Loaded->GetShaderStageBinaries(EMaterialShaderStage::Pixel).empty());
        EXPECT_EQ(Loaded->Stages[0].SourceHash, 0x5678ull);

        GShaderLibrary = Previous;
    }

    TEST_F(MaterialShaderCache, ASaveWithNoKeptSpirvWritesNoTemplateStamp)
    {
        CMaterial* Source = NewObject<CMaterial>();
        ASSERT_NE(Source, nullptr);

        const TVector<uint32> FromGraph = MakeBlob(3);
        Source->SetStageBinaries(EMaterialShaderStage::Pixel, TSpan<const uint32>(FromGraph.data(), FromGraph.size()), 0x1234ull);
        Source->CompiledTemplateHash = 42;
        Source->MaterialType = EMaterialType::Terrain;

        CMaterial* Loaded = SaveAndLoad(Source);
        ASSERT_NE(Loaded, nullptr);

        ASSERT_EQ(Loaded->MaterialType, EMaterialType::Terrain) << "the round trip itself has to work for the rest to mean anything";
        EXPECT_EQ(Loaded->CompiledTemplateHash, 0ull);
        EXPECT_EQ(Source->CompiledTemplateHash, 42ull);
    }
}
