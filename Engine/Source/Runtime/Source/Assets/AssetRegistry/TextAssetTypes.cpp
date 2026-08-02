#include "RuntimePCH.h"
#include "TextAssetTypes.h"

#include "FileSystem/FileSystem.h"

namespace Lumina::TextAsset
{
    namespace
    {
        // Case-insensitive compare of an extension token against a lowercase literal.
        bool IEquals(FStringView A, FStringView B)
        {
            if (A.size() != B.size()) return false;
            for (size_t i = 0; i < A.size(); ++i)
            {
                char Ca = A[i]; if (Ca >= 'A' && Ca <= 'Z') Ca = char(Ca + ('a' - 'A'));
                if (Ca != B[i]) return false;
            }
            return true;
        }
    }

    ETextAssetKind KindFromExtension(FStringView Ext)
    {
        // Accept with or without the leading dot.
        if (!Ext.empty() && Ext[0] == '.') Ext = Ext.substr(1);

        if (IEquals(Ext, "luau")) return ETextAssetKind::LuaScript;
        if (IEquals(Ext, "rml"))  return ETextAssetKind::RmlDocument;
        if (IEquals(Ext, "rcss")) return ETextAssetKind::RmlStyleSheet;
        return ETextAssetKind::None;
    }

    ETextAssetKind KindFromPath(FStringView Path)
    {
        return KindFromExtension(VFS::Extension(Path));
    }

    FStringView ExtensionForKind(ETextAssetKind Kind)
    {
        switch (Kind)
        {
        case ETextAssetKind::LuaScript:     return ".luau";
        case ETextAssetKind::RmlDocument:   return ".rml";
        case ETextAssetKind::RmlStyleSheet: return ".rcss";
        default:                            return {};
        }
    }

    FStringView DisplayName(ETextAssetKind Kind)
    {
        switch (Kind)
        {
        case ETextAssetKind::LuaScript:     return "Lua Script";
        case ETextAssetKind::RmlDocument:   return "RML Document";
        case ETextAssetKind::RmlStyleSheet: return "RML Stylesheet";
        default:                            return "None";
        }
    }

    bool IsTextAssetPath(FStringView Path)
    {
        return KindFromPath(Path) != ETextAssetKind::None;
    }

    TVector<ETextAssetKind> ParseAssetTypeMeta(FStringView Meta)
    {
        TVector<ETextAssetKind> Out;

        auto AddUnique = [&Out](ETextAssetKind K)
        {
            if (K == ETextAssetKind::None) return;
            for (ETextAssetKind Existing : Out) { if (Existing == K) return; }
            Out.push_back(K);
        };

        // Split on commas / whitespace.
        size_t i = 0;
        const size_t N = Meta.size();
        while (i < N)
        {
            while (i < N && (Meta[i] == ',' || Meta[i] == ' ' || Meta[i] == '\t')) ++i;
            size_t Start = i;
            while (i < N && Meta[i] != ',' && Meta[i] != ' ' && Meta[i] != '\t') ++i;
            if (i <= Start) continue;

            FStringView Token = Meta.substr(Start, i - Start);

            if (IEquals(Token, "any") || IEquals(Token, "*"))
            {
                Out.clear();
                AddUnique(ETextAssetKind::LuaScript);
                AddUnique(ETextAssetKind::RmlDocument);
                AddUnique(ETextAssetKind::RmlStyleSheet);
                return Out;
            }

            // Extension/alias forms first.
            ETextAssetKind K = KindFromExtension(Token);
            if (K == ETextAssetKind::None)
            {
                if (IEquals(Token, "script") || IEquals(Token, "lua")) K = ETextAssetKind::LuaScript;
                else if (IEquals(Token, "document")) K = ETextAssetKind::RmlDocument;
                else if (IEquals(Token, "stylesheet") || IEquals(Token, "css")) K = ETextAssetKind::RmlStyleSheet;
            }
            AddUnique(K);
        }

        // Empty meta == accept everything.
        if (Out.empty())
        {
            AddUnique(ETextAssetKind::LuaScript);
            AddUnique(ETextAssetKind::RmlDocument);
            AddUnique(ETextAssetKind::RmlStyleSheet);
        }
        return Out;
    }
}
