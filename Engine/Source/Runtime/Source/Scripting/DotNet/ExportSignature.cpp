#include "RuntimePCH.h"

#include "ExportSignature.h"
#include "NameSizeRegistry.h"
#include "Scripting/DotNet/DotNetExport.h"

namespace Lumina::DotNet
{
    void RegisterExportSignatures(const FExportSignatureEntry* Entries, int32 Count)
    {
        if (Entries == nullptr)
        {
            return;
        }

        for (int32 Index = 0; Index < Count; ++Index)
        {
            NameSizeRegistry::Register(ENameSizeRegistry::ExportSignature, Entries[Index].Name, Entries[Index].Size);
        }
    }

    int32 GetExportSignature(const char* Name, int32 Len)
    {
        return NameSizeRegistry::Find(ENameSizeRegistry::ExportSignature, Name, Len);
    }
}

// Negative when the export registered no signature, which the managed side reports rather than treating as a match.
LUMINA_DOTNET_EXPORT(int32, ExportSignature)(const char* Name, int32 Len)
{
    return ::Lumina::DotNet::GetExportSignature(Name, Len);
}

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(ExportSignature)
);
