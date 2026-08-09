#pragma once

#include "Tools/Import/Importer.h"
#include "AudioImporter.generated.h"

namespace Lumina
{
    REFLECT()
    class EDITOR_API CAudioImporter : public CImporter
    {
        GENERATED_BODY()
    public:

        FStringView GetImporterDisplayName() const override { return "Audio"; }

        void GetSupportedExtensions(TVector<FStringView>& OutExtensions) const override
        {
            OutExtensions.insert(OutExtensions.end(), { ".wav" });
        }

        void BuildAssets(const FImportRequest& Request, FImportResult& OutResult, FScopedSlowTask* Progress) override;

        bool CanReimport(const CStruct* AssetClass) const override;
        bool ReimportAsset(CObject* Asset, const FImportRequest& Request, FScopedSlowTask* Progress) override;
        FString GetReimportSourcePath(const CObject* Asset) const override;
    };
}
