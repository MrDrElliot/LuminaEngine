#include "RuntimePCH.h"
#include "AssetData.h"

namespace Lumina
{
    FStringView LexToString(EDependencyType T)
    {
        switch (T)
        {
            case EDependencyType::Hard:       return "Hard";
            case EDependencyType::Soft:       return "Soft";
            case EDependencyType::Script:     return "Script";
            case EDependencyType::Owned:      return "Owned";
            case EDependencyType::EditorOnly: return "EditorOnly";
            case EDependencyType::Generated:  return "Generated";
            default:                          return "Unknown";
        }
    }
}
