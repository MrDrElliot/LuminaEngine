#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Tools/Import/ImportHelpers.h"
#include "MeshFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CMeshFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        FString GetAssetName() const override { return "Mesh"; }
        FStringView GetDefaultAssetCreationName() override { return "NewMesh"; }

        FString GetAssetDescription() const override { return "A mesh."; }
        CClass* GetAssetClass() const override { return CStaticMesh::StaticClass(); }
        bool CanImport() override { return true; }
        bool IsExtensionSupported(FStringView Ext) override { return Ext == ".gltf" || Ext == ".glb" || Ext == ".obj" || Ext == ".fbx"; }
        void GetSupportedExtensions(TVector<FStringView>& OutExtensions) const override
        {
            OutExtensions.insert(OutExtensions.end(), { ".gltf", ".glb", ".obj", ".fbx" });
        }

        bool CanReimport(const CStruct* AssetClass) const override;
        bool TryReimport(CObject* Asset, const FFixedString& SourceFile, const Import::FImportSettings* Settings) override;
        FString GetReimportSourcePath(const CObject* Asset) const override;

        bool HasImportDialogue() const override { return true; }
        void PrepareImportAsync(const FFixedString& RawPath, const FFixedString& DestinationPath, FImportPrepareCallback OnReady) override;
        void DrawImportSettings(const FFixedString& RawPath, Import::FImportSettings& Settings) override;
        void CommitImportSettings(Import::FImportSettings& Settings) override;
        void TryImport(const FFixedString& RawPath, const FFixedString& DestinationPath, const Import::FImportSettings* Settings) override;
        
    };
}
