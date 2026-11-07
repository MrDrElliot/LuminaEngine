#include "RuntimePCH.h"
#include "UIDocumentRef.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Serialization/NetArchive.h"
#include "FileSystem/FileSystem.h"

namespace Lumina
{
    void SUIDocumentRef::NetSerialize(FNetArchive& Ar)
    {
        if (Ar.IsWriting())
        {
            // An indexed path uses a compact net index, with path and GUID exported once via AssetExport.
            if (Ar.DocumentRefToNetIndex)
            {
                std::ignore = ResolvePath(); // heal/back-fill the GUID so the export keys on a stable identity
                WriteVarUInt(Ar, IsNull() ? 0u : Ar.DocumentRefToNetIndex(*this));
                return;
            }

            Ar << Path;
            Ar << Guid;
        }
        else
        {
            if (Ar.NetIndexToDocumentRef)
            {
                const uint32 Index = ReadVarUInt(Ar);
                Reset();
                if (Index != 0)
                {
                    Ar.NetIndexToDocumentRef(Index, *this);
                }
                return;
            }

            Ar << Path;
            Ar << Guid;
        }
    }

    FGuid SUIDocumentRef::GetGuid() const
    {
        if (Guid.IsEmpty())
        {
            return FGuid();
        }
        if (Guid != ParsedGuidText)
        {
            const TOptional<FGuid> Parsed = FGuid::TryParse(FStringView(*Guid, Guid.Len()));
            ParsedGuid = Parsed.IsSet() ? *Parsed : FGuid();
            ParsedGuidText = Guid;
        }
        return ParsedGuid;
    }

    FStringView SUIDocumentRef::ResolvePath() const
    {
        FAssetRegistry& Registry = FAssetRegistry::Get();

        // A valid GUID locates the asset even after a rename, and Path heals in place.
        const FGuid G = GetGuid();
        if (G.IsValid())
        {
            if (FTextAssetData* Data = Registry.GetTextAssetByGUID(G))
            {
                const FStringView Current(*Data->Path, Data->Path.Len());
                if (Current != GetPath())
                {
                    const_cast<SUIDocumentRef*>(this)->Path = FStringView(Current.GetData(), Current.Len());
                }
                return GetPath();
            }
        }

        // Back-fills the GUID from the record, so legacy path-only data self-heals once a sidecar exists.
        if (!Path.IsEmpty())
        {
            if (FTextAssetData* Data = Registry.GetTextAssetByPath(GetPath()))
            {
                const_cast<SUIDocumentRef*>(this)->Guid = Data->Guid.ToString();
            }
        }

        return GetPath();
    }

    bool SUIDocumentRef::ReadText(FString& OutText) const
    {
        const FStringView ResolvedPath = ResolvePath();
        if (ResolvedPath.IsEmpty())
        {
            return false;
        }
        return VFS::ReadFile(OutText, ResolvedPath);
    }

    void SUIDocumentRef::SetPath(FStringView InPath)
    {
        Path = FStringView(InPath.GetData(), InPath.Len());
        Guid.Reset();

        // Mint/resolve a sidecar identity now (editor context).
        const FGuid G = FAssetRegistry::Get().EnsureTextAsset(InPath);
        if (G.IsValid())
        {
            Guid = G.ToString();
        }
    }

    void SUIDocumentRef::Set(FStringView InPath, const FGuid& InGuid)
    {
        Path = FStringView(InPath.GetData(), InPath.Len());
        Guid = InGuid.IsValid() ? InGuid.ToString() : FString();
    }
}
