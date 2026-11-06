#include "RuntimePCH.h"
#include "TextAssetSidecar.h"

#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

namespace Lumina::TextAssetSidecar
{
    namespace
    {
        constexpr uint32 kSidecarTag     = 0x4C4D5441; // 'LMTA'
        constexpr uint16 kSidecarVersion = 1;

        // Most-specific first, so the hidden .lmeta lands inside Content rather than the engine root.
        TVector<FFixedString> GatherContentRoots()
        {
            TVector<FFixedString> Roots;
            Roots.Emplace(FFixedString("/Engine/Resources/Content"));
            Roots.Emplace(FFixedString("/Game/Content"));
            Roots.Emplace(FFixedString("/Game/Scripts"));

            for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
            {
                if (Plugin == nullptr)            continue;
                if (!Plugin->IsContentMounted())  continue;
                Roots.Emplace(FFixedString(*Plugin->GetMountAlias()));
            }
            return Roots;
        }

        // Find the content root owning Path; returns its length (root prefix) or kIndexNone. The root is copied out, since the list it came from is a temporary.
        int32 FindContentRoot(FStringView Path, FFixedString& OutRoot)
        {
            int32 BestLen = Constants::kIndexNone;
            for (const FFixedString& Root : GatherContentRoots())
            {
                const FStringView RootView(*Root, Root.Len());
                if (RootView.IsEmpty() || RootView.Len() > Path.Len()) continue;
                if (!Path.StartsWith(RootView)) continue;
                // Must land on a path boundary.
                if (Path.Len() > RootView.Len() && Path[RootView.Len()] != '/') continue;
                if (BestLen == Constants::kIndexNone || RootView.Len() > BestLen)
                {
                    BestLen = RootView.Len();
                    OutRoot = Root;
                }
            }
            return BestLen;
        }
    }

    FFixedString PathFor(FStringView ContentVirtualPath)
    {
        FFixedString Root;
        const int32 RootLen = FindContentRoot(ContentVirtualPath, Root);
        if (RootLen == Constants::kIndexNone)
        {
            return {};
        }

        // Relative subpath under the root (no leading slash).
        FStringView Relative = ContentVirtualPath.Mid(RootLen);
        while (!Relative.IsEmpty() && Relative[0] == '/') Relative.RemovePrefix(1);

        FFixedString Out(*Root, Root.Len());
        Out.Append("/");
        Out.Append(kMetaDirName.GetData(), kMetaDirName.Len());
        Out.Append("/");
        Out.Append(Relative.GetData(), Relative.Len());
        Out.Append(kSidecarExt.GetData(), kSidecarExt.Len());
        return Out;
    }

    bool IsSidecarPath(FStringView Path)
    {
        // Any path component equal to the meta dir name.
        constexpr FStringView Needle("/.lmeta");
        int32 Pos = 0;
        while ((Pos = Path.Find(Needle, Pos)) != Constants::kIndexNone)
        {
            const int32 After = Pos + Needle.Len();
            if (After == Path.Len() || Path[After] == '/')
            {
                return true;
            }
            Pos = After;
        }
        return false;
    }

    bool Read(FStringView ContentVirtualPath, FGuid& OutGuid, ETextAssetKind* OutKind)
    {
        const FFixedString SidecarPath = PathFor(ContentVirtualPath);
        if (SidecarPath.IsEmpty()) return false;

        TVector<uint8> Bytes;
        if (!VFS::ReadFile(Bytes, FStringView(*SidecarPath, SidecarPath.Len())))
        {
            return false;
        }
        if (static_cast<size_t>(Bytes.Num()) < sizeof(uint32)) return false;

        FMemoryReader Reader(Bytes);
        uint32 Tag = 0; Reader << Tag;
        if (Tag != kSidecarTag) return false;

        uint16 Version = 0; Reader << Version;
        FGuid Guid; Reader << Guid;
        uint8 Kind = 0; Reader << Kind;

        if (!Guid.IsValid()) return false;

        OutGuid = Guid;
        if (OutKind) *OutKind = (ETextAssetKind)Kind;
        return true;
    }

    bool Write(FStringView ContentVirtualPath, const FGuid& Guid, ETextAssetKind Kind)
    {
        const FFixedString SidecarPath = PathFor(ContentVirtualPath);
        if (SidecarPath.IsEmpty() || !Guid.IsValid()) return false;

        TVector<uint8> Bytes;
        FMemoryWriter Writer(Bytes);
        uint32 Tag = kSidecarTag;       Writer << Tag;
        uint16 Version = kSidecarVersion; Writer << Version;
        FGuid GuidCopy = Guid;          Writer << GuidCopy;
        uint8 KindByte = (uint8)Kind;   Writer << KindByte;

        return VFS::AtomicWriteFile(FStringView(*SidecarPath, SidecarPath.Len()), Bytes);
    }

    FGuid ReadOrMint(FStringView ContentVirtualPath, ETextAssetKind Kind)
    {
        FGuid Guid;
        if (Read(ContentVirtualPath, Guid))
        {
            return Guid;
        }

        Guid = FGuid::New();
        if (!Write(ContentVirtualPath, Guid, Kind))
        {
            LOG_WARN("TextAssetSidecar: failed to persist identity for {}; reference stability not guaranteed this session", ContentVirtualPath);
        }
        return Guid;
    }

    bool Move(FStringView OldContentPath, FStringView NewContentPath)
    {
        FGuid Guid;
        ETextAssetKind Kind = ETextAssetKind::None;
        if (!Read(OldContentPath, Guid, &Kind))
        {
            // Nothing recorded for the old path; nothing to relocate.
            return false;
        }

        // Recover kind from the new extension if the old sidecar lacked it.
        if (Kind == ETextAssetKind::None)
        {
            Kind = TextAsset::KindFromPath(NewContentPath);
        }

        const bool bWrote = Write(NewContentPath, Guid, Kind);
        Delete(OldContentPath);
        return bWrote;
    }

    bool Delete(FStringView ContentVirtualPath)
    {
        const FFixedString SidecarPath = PathFor(ContentVirtualPath);
        if (SidecarPath.IsEmpty()) return false;
        const FStringView View(*SidecarPath, SidecarPath.Len());
        if (!VFS::Exists(View)) return false;
        return VFS::Remove(View);
    }
}
