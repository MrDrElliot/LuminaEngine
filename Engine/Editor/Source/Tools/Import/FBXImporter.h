#pragma once

#include "Tools/Import/MeshImporter.h"
#include "FBXImporter.generated.h"

namespace Lumina
{
    REFLECT()
    class EDITOR_API CFBXImporter : public CMeshImporter
    {
        GENERATED_BODY()
    public:

        FStringView GetImporterDisplayName() const override { return "FBX"; }

        void GetSupportedExtensions(TVector<FStringView>& OutExtensions) const override
        {
            OutExtensions.insert(OutExtensions.end(), { ".fbx" });
        }

    protected:

        bool ParseMeshSource(const FImportRequest& Request,
                             const Import::Mesh::FMeshImportOptions& Options,
                             Import::Mesh::FMeshImportData& OutData,
                             FString& OutError,
                             FScopedSlowTask* Progress) override;
    };
}
