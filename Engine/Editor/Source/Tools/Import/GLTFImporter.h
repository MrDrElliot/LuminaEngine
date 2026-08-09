#pragma once

#include "Tools/Import/MeshImporter.h"
#include "GLTFImporter.generated.h"

struct cgltf_data;

namespace Lumina
{
    REFLECT()
    class EDITOR_API CGLTFImporter : public CMeshImporter
    {
        GENERATED_BODY()
    public:

        /**
         * Collapse meshes whose primitives reference the same source buffers into one asset. Exporters
         * routinely emit a separate mesh per object even when the geometry is shared; without this a scene
         * of 200k duplicated props imports 200k identical mesh assets.
         */
        PROPERTY(Editable, Category = "Deduplication")
        bool bDeduplicateMeshes = true;

        /** Collapse materials whose parameters and texture bindings are identical into one asset. */
        PROPERTY(Editable, Category = "Deduplication")
        bool bDeduplicateMaterials = true;

        /** Import every scene in the file rather than only the default one. */
        PROPERTY(Editable, Category = "Scene")
        bool bImportAllScenes = false;

        FStringView GetImporterDisplayName() const override { return "glTF"; }

        void GetSupportedExtensions(TVector<FStringView>& OutExtensions) const override
        {
            OutExtensions.insert(OutExtensions.end(), { ".gltf", ".glb" });
        }

        // Embedded image payloads are views into the parsed buffers, so the parse outlives ParseMeshSource
        // and is only released once the import is committed or cancelled.
        void ReleaseSourceData() override;

    protected:

        bool ParseMeshSource(const FImportRequest& Request,
                             const Import::Mesh::FMeshImportOptions& Options,
                             Import::Mesh::FMeshImportData& OutData,
                             FString& OutError,
                             FScopedSlowTask* Progress) override;

    private:

        cgltf_data* ParsedData = nullptr;
    };
}
