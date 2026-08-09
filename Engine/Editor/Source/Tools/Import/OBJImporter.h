#pragma once

#include "Tools/Import/MeshImporter.h"
#include "OBJImporter.generated.h"

namespace Lumina
{
    REFLECT()
    class EDITOR_API COBJImporter : public CMeshImporter
    {
        GENERATED_BODY()
    public:

        FStringView GetImporterDisplayName() const override { return "Wavefront OBJ"; }

        void GetSupportedExtensions(TVector<FStringView>& OutExtensions) const override
        {
            OutExtensions.insert(OutExtensions.end(), { ".obj" });
        }

    protected:

        bool ParseMeshSource(const FImportRequest& Request,
                             const Import::Mesh::FMeshImportOptions& Options,
                             Import::Mesh::FMeshImportData& OutData,
                             FString& OutError,
                             FScopedSlowTask* Progress) override;
    };
}
