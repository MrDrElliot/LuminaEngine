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
            if (A.Len() != B.Len()) return false;
            for (int32 i = 0; i < A.Len(); ++i)
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
        if (!Ext.IsEmpty() && Ext[0] == '.') Ext = Ext.Mid(1);

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
        case ETextAssetKind::RmlDocument:   return ".rml";
        case ETextAssetKind::RmlStyleSheet: return ".rcss";
        default:                            return {};
        }
    }

    FStringView DisplayName(ETextAssetKind Kind)
    {
        switch (Kind)
        {
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
            Out.Add(K);
        };

        // Split on commas / whitespace.
        int32 i = 0;
        const int32 N = Meta.Len();
        while (i < N)
        {
            while (i < N && (Meta[i] == ',' || Meta[i] == ' ' || Meta[i] == '\t')) ++i;
            int32 Start = i;
            while (i < N && Meta[i] != ',' && Meta[i] != ' ' && Meta[i] != '\t') ++i;
            if (i <= Start) continue;

            FStringView Token = Meta.Mid(Start, i - Start);

            if (IEquals(Token, "any") || IEquals(Token, "*"))
            {
                Out.Reset();
                AddUnique(ETextAssetKind::RmlDocument);
                AddUnique(ETextAssetKind::RmlStyleSheet);
                return Out;
            }

            // Extension/alias forms first.
            ETextAssetKind K = KindFromExtension(Token);
            if (K == ETextAssetKind::None)
            {
                if (IEquals(Token, "document")) K = ETextAssetKind::RmlDocument;
                else if (IEquals(Token, "stylesheet") || IEquals(Token, "css")) K = ETextAssetKind::RmlStyleSheet;
            }
            AddUnique(K);
        }

        // Empty meta == accept everything.
        if (Out.IsEmpty())
        {
            AddUnique(ETextAssetKind::RmlDocument);
            AddUnique(ETextAssetKind::RmlStyleSheet);
        }
        return Out;
    }
}
