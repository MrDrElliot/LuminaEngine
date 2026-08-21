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
            Roots.emplace_back(FFixedString("/Engine/Resources/Content"));
            Roots.emplace_back(FFixedString("/Game/Content"));
            Roots.emplace_back(FFixedString("/Game/Scripts"));

            for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
            {
                if (Plugin == nullptr)            continue;
                if (!Plugin->IsContentMounted())  continue;
                Roots.emplace_back(FFixedString(Plugin->GetMountAlias().c_str()));
            }
            return Roots;
        }

        // Find the content root owning Path; returns its length (root prefix) or npos.
        size_t FindContentRoot(FStringView Path, FStringView& OutRoot)
        {
            size_t BestLen = FStringView::npos;
            for (const FFixedString& Root : GatherContentRoots())
            {
                const FStringView RootView(Root.c_str(), Root.size());
                if (RootView.empty() || RootView.size() > Path.size()) continue;
                if (!Path.starts_with(RootView)) continue;
                // Must land on a path boundary.
                if (Path.size() > RootView.size() && Path[RootView.size()] != '/') continue;
                if (BestLen == FStringView::npos || RootView.size() > BestLen)
                {
                    BestLen = RootView.size();
                    OutRoot = RootView;
                }
            }
            return BestLen;
        }
    }

    FFixedString PathFor(FStringView ContentVirtualPath)
    {
        FStringView Root;
        const size_t RootLen = FindContentRoot(ContentVirtualPath, Root);
        if (RootLen == FStringView::npos)
        {
            return {};
        }

        // Relative subpath under the root (no leading slash).
        FStringView Relative = ContentVirtualPath.substr(RootLen);
        while (!Relative.empty() && Relative.front() == '/') Relative.remove_prefix(1);

        FFixedString Out(Root.data(), Root.size());
        Out.append("/");
        Out.append(kMetaDirName.data(), kMetaDirName.size());
        Out.append("/");
        Out.append(Relative.data(), Relative.size());
        Out.append(kSidecarExt.data(), kSidecarExt.size());
        return Out;
    }

    bool IsSidecarPath(FStringView Path)
    {
        // Any path component equal to the meta dir name.
        constexpr FStringView Needle("/.lmeta");
        size_t Pos = 0;
        while ((Pos = Path.find(Needle, Pos)) != FStringView::npos)
        {
            const size_t After = Pos + Needle.size();
            if (After == Path.size() || Path[After] == '/')
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
        if (SidecarPath.empty()) return false;

        TVector<uint8> Bytes;
        if (!VFS::ReadFile(Bytes, FStringView(SidecarPath.c_str(), SidecarPath.size())))
        {
            return false;
        }
        if (Bytes.size() < sizeof(uint32)) return false;

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
        if (SidecarPath.empty() || !Guid.IsValid()) return false;

        TVector<uint8> Bytes;
        FMemoryWriter Writer(Bytes);
        uint32 Tag = kSidecarTag;       Writer << Tag;
        uint16 Version = kSidecarVersion; Writer << Version;
        FGuid GuidCopy = Guid;          Writer << GuidCopy;
        uint8 KindByte = (uint8)Kind;   Writer << KindByte;

        return VFS::AtomicWriteFile(FStringView(SidecarPath.c_str(), SidecarPath.size()), Bytes);
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
        if (SidecarPath.empty()) return false;
        const FStringView View(SidecarPath.c_str(), SidecarPath.size());
        if (!VFS::Exists(View)) return false;
        return VFS::Remove(View);
    }
}
