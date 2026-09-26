#include "RuntimePCH.h"

#include "ExportSignature.h"
#include "Scripting/DotNet/DotNetExport.h"

#include <string>
#include <unordered_map>

namespace Lumina::DotNet
{
    namespace
    {
        // A function-local static, so the map exists before any self-registration runs.
        std::unordered_map<std::string, int32>& SignatureRegistry()
        {
            static std::unordered_map<std::string, int32> Map;
            return Map;
        }
    }

    void RegisterExportSignatures(const FExportSignatureEntry* Entries, int32 Count)
    {
        if (Entries == nullptr)
        {
            return;
        }

        for (int32 Index = 0; Index < Count; ++Index)
        {
            SignatureRegistry()[std::string(Entries[Index].Name)] = Entries[Index].Size;
        }
    }

    int32 GetExportSignature(const char* Name, int32 Len)
    {
        if (Name == nullptr || Len <= 0)
        {
            return -1;
        }

        const std::unordered_map<std::string, int32>& Map = SignatureRegistry();
        auto It = Map.find(std::string(Name, (size_t)Len));
        return It != Map.end() ? It->second : -1;
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
