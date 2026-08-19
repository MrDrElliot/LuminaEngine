#pragma once

#include "Tools/Import/MeshImporter.h"
#include "FBXImporter.generated.h"

struct ufbx_scene;

namespace Lumina
{
    REFLECT()
    class EDITOR_API CFBXImporter : public CMeshImporter
    {
        GENERATED_BODY()
    public:

        /**
         * Collapse meshes whose geometry and material assignment are identical into one asset. FBX scenes
         * routinely carry a separate mesh per placed object even when the geometry is shared.
         */
        PROPERTY(Editable, Category = "Deduplication")
        bool bDeduplicateMeshes = true;

        /** Collapse materials whose parameters and texture bindings are identical into one asset. */
        PROPERTY(Editable, Category = "Deduplication")
        bool bDeduplicateMaterials = true;

        FStringView GetImporterDisplayName() const override { return "FBX"; }

        void GetSupportedExtensions(TVector<FStringView>& OutExtensions) const override
        {
            OutExtensions.insert(OutExtensions.end(), { ".fbx" });
        }

        // Embedded image payloads are views into the loaded ufbx scene, so the parse outlives
        // ParseMeshSource and is only released once the import is committed or canceled.
        void ReleaseSourceData() override;

    protected:

        bool ParseMeshSource(const FImportRequest& Request,
                             const Import::Mesh::FMeshImportOptions& Options,
                             Import::Mesh::FMeshImportData& OutData,
                             FString& OutError,
                             FScopedSlowTask* Progress) override;

    private:

        ufbx_scene* Scene = nullptr;

        /** The file bytes ufbx parsed. ufbx_string/ufbx_blob members point into this, so it outlives the scene. */
        TVector<uint8> SourceBlob;
    };
}
