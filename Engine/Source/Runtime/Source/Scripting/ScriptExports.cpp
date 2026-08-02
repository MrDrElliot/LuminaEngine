#include "RuntimePCH.h"
#include "ScriptExports.h"

#include <cstdlib>

namespace Lumina::Scripting
{
    const FString* FScriptExportMeta::Find(const FName& Key) const
    {
        for (const FScriptExportMetaArg& Arg : Entries)
        {
            if (Arg.Key == Key)
            {
                return &Arg.Value;
            }
        }
        return nullptr;
    }

    void FScriptExportMeta::Set(const FName& Key, const FString& Value)
    {
        for (FScriptExportMetaArg& Arg : Entries)
        {
            if (Arg.Key == Key)
            {
                Arg.Value = Value;
                return;
            }
        }
        Entries.push_back(FScriptExportMetaArg{ Key, Value });
    }

    bool FScriptExportMeta::GetNumber(const FName& Key, double& OutValue) const
    {
        const FString* Value = Find(Key);
        if (!Value || Value->empty())
        {
            return false;
        }
        char* End = nullptr;
        const double Parsed = std::strtod(Value->c_str(), &End);
        if (End == Value->c_str())
        {
            return false;
        }
        OutValue = Parsed;
        return true;
    }
}
