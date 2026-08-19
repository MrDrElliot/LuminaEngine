#include "Core/Threading/Thread.h"
#include "EditorPCH.h"
#include "Importer.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "FileSystem/FileSystem.h"

namespace Lumina
{
    namespace
    {
        CImporterRegistry* GImporterRegistry = nullptr;

        bool ExtensionsMatch(FStringView A, FStringView B)
        {
            if (A.size() != B.size())
            {
                return false;
            }
            for (size_t i = 0; i < A.size(); ++i)
            {
                char CA = A[i];
                char CB = B[i];
                if (CA >= 'A' && CA <= 'Z') { CA = (char)(CA + ('a' - 'A')); }
                if (CB >= 'A' && CB <= 'Z') { CB = (char)(CB + ('a' - 'A')); }
                if (CA != CB)
                {
                    return false;
                }
            }
            return true;
        }
    }

    void CImporter::PostCreateCDO()
    {
        CImporterRegistry::Get().RegisterImporter(this);
    }

    bool CImporter::SupportsExtension(FStringView Ext) const
    {
        TVector<FStringView> Extensions;
        GetSupportedExtensions(Extensions);

        for (const FStringView& Supported : Extensions)
        {
            if (ExtensionsMatch(Supported, Ext))
            {
                return true;
            }
        }
        return false;
    }

    CImporterRegistry& CImporterRegistry::Get()
    {
        static FOnceFlag Flag;
        CallOnce(Flag, []()
        {
            GImporterRegistry = NewObject<CImporterRegistry>();
            GImporterRegistry->AddToRoot();
        });

        return *GImporterRegistry;
    }

    void CImporterRegistry::RegisterImporter(CImporter* Importer)
    {
        // The base CDO claims nothing, so keeping it out of the list means every lookup below can skip
        // the "is this actually a concrete importer" test.
        if (Importer == nullptr || Importer->GetClass() == CImporter::StaticClass())
        {
            return;
        }
        Importers.push_back(Importer);
    }

    CImporter* CImporterRegistry::FindImporterForExtension(FStringView Ext) const
    {
        CImporter* Best = nullptr;
        for (CImporter* Importer : Importers)
        {
            if (!Importer->SupportsExtension(Ext))
            {
                continue;
            }
            if (Best == nullptr || Importer->GetPriority() > Best->GetPriority())
            {
                Best = Importer;
            }
        }
        return Best;
    }

    CImporter* CImporterRegistry::FindReimporter(const CStruct* AssetClass) const
    {
        if (AssetClass == nullptr)
        {
            return nullptr;
        }

        CImporter* Best = nullptr;
        for (CImporter* Importer : Importers)
        {
            if (!Importer->CanReimport(AssetClass))
            {
                continue;
            }
            if (Best == nullptr || Importer->GetPriority() > Best->GetPriority())
            {
                Best = Importer;
            }
        }
        return Best;
    }

    CImporter* CImporterRegistry::CreateImporterFor(FStringView SourcePath) const
    {
        CImporter* CDO = FindImporterForExtension(VFS::Extension(SourcePath));
        return CDO != nullptr ? CreateImporterOfClass(CDO->GetClass()) : nullptr;
    }

    CImporter* CImporterRegistry::CreateImporterOfClass(CClass* ImporterClass)
    {
        if (ImporterClass == nullptr)
        {
            return nullptr;
        }

        CImporter* Instance = NewObject<CImporter>(ImporterClass);
        if (Instance == nullptr)
        {
            return nullptr;
        }

        // Seeded from the CDO rather than from the C++ defaults, so settings the user chose last time carry
        // into the next import of the same type (CommitSettingsToDefaults writes them back).
        if (CImporter* CDO = ImporterClass->GetDefaultObject<CImporter>())
        {
            CDO->CopyPropertiesTo(Instance);
        }

        Instance->AddToRoot();
        return Instance;
    }

    void CImporterRegistry::DestroyImporter(CImporter* Importer)
    {
        if (Importer == nullptr)
        {
            return;
        }

        Importer->ReleaseSourceData();
        Importer->RemoveFromRoot();
        Importer->ConditionalBeginDestroy();
    }

    bool CImporterRegistry::IsExtensionSupported(FStringView Ext) const
    {
        return FindImporterForExtension(Ext) != nullptr;
    }

    FFixedString CImporterRegistry::BuildFileDialogFilter() const
    {
        TVector<FStringView> Extensions;
        for (CImporter* Importer : Importers)
        {
            Importer->GetSupportedExtensions(Extensions);
        }

        if (Extensions.empty())
        {
            return FFixedString("All Files (*.*)\0*.*\0\0", 21);
        }

        FFixedString Patterns;
        for (const FStringView& Ext : Extensions)
        {
            if (!Patterns.empty())
            {
                Patterns.append(";");
            }
            Patterns.append("*").append(Ext.data(), Ext.length());
        }

        // Win32 filters are a double-null-terminated run of null-separated pairs.
        FFixedString Filter;
        Filter.append("Supported Files (").append(Patterns).append(")");
        Filter.push_back('\0');
        Filter.append(Patterns);
        Filter.push_back('\0');
        Filter.append("All Files (*.*)");
        Filter.push_back('\0');
        Filter.append("*.*");
        Filter.push_back('\0');
        Filter.push_back('\0');

        return Filter;
    }
}
