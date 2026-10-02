#include "CSharpAssetScan.h"

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/HashTable.h"
#include "Containers/StringFormat.h"
#include "FileSystem/FileSystem.h"

namespace Lumina
{
    namespace
    {
        bool IsCSharpSource(FStringView VirtualPath)
        {
            return VFS::Extension(VirtualPath) == ".cs";
        }

        // Scripts read tables like item lists from JSON and load the meshes those name, so the data is scanned with the code.
        bool IsScannedSource(FStringView VirtualPath)
        {
            const FStringView Extension = VFS::Extension(VirtualPath);
            return (Extension == ".cs" || Extension == ".json")
                && VirtualPath.find("/obj/") == FStringView::npos
                && VirtualPath.find("/bin/") == FStringView::npos
                && VirtualPath.find("/Config/") == FStringView::npos;
        }

        // Reads one literal from its opening quote, returning the index just past its end.
        size_t ReadLiteral(FStringView Src, size_t Quote, bool bVerbatim, bool bInterpolated, FString& Out, bool& bOutCut)
        {
            size_t i = Quote + 1;
            while (i < Src.size())
            {
                const char c = Src[i];
                if (bVerbatim && c == '"' && i + 1 < Src.size() && Src[i + 1] == '"')
                {
                    Out.push_back('"');
                    i += 2;
                    continue;
                }
                if (c == '"')
                {
                    return i + 1;
                }
                if (!bVerbatim && c == '\\' && i + 1 < Src.size())
                {
                    Out.push_back(Src[i + 1]);
                    i += 2;
                    continue;
                }
                if (bInterpolated && c == '{')
                {
                    if (i + 1 < Src.size() && Src[i + 1] == '{')
                    {
                        Out.push_back('{');
                        i += 2;
                        continue;
                    }
                    bOutCut = true;
                }
                if (!bOutCut)
                {
                    Out.push_back(c);
                }
                if (!bVerbatim && c == '\n')
                {
                    return i + 1;
                }
                ++i;
            }
            return i;
        }

        bool LooksLikeContentPath(FStringView Literal)
        {
            return Literal.size() > 1 && Literal[0] == '/' && Literal.find(' ') == FStringView::npos;
        }

        // A mount root such as /Game or /Game/Content would cook the whole project, so a folder must sit below one.
        bool IsSpecificFolder(FStringView Folder)
        {
            while (!Folder.empty() && Folder.back() == '/')
            {
                Folder.remove_suffix(1);
            }
            size_t Segments = 0;
            for (const char c : Folder)
            {
                Segments += c == '/' ? 1 : 0;
            }
            return Segments >= 3;
        }
    }

    TVector<FString> FCSharpAssetScan::ExtractCandidates(FStringView Src)
    {
        TVector<FString> Out;
        size_t i = 0;
        while (i < Src.size())
        {
            const char c = Src[i];

            if (c == '/' && i + 1 < Src.size() && Src[i + 1] == '/')
            {
                const size_t End = Src.find('\n', i);
                i = End == FStringView::npos ? Src.size() : End;
                continue;
            }
            if (c == '/' && i + 1 < Src.size() && Src[i + 1] == '*')
            {
                const size_t End = Src.find("*/", i + 2);
                i = End == FStringView::npos ? Src.size() : End + 2;
                continue;
            }
            if (c == '\'')
            {
                // A char literal, skipped so a quote inside it cannot open a string.
                const size_t Close = Src.find('\'', i + (i + 1 < Src.size() && Src[i + 1] == '\\' ? 3 : 2));
                i = Close == FStringView::npos ? Src.size() : Close + 1;
                continue;
            }

            bool bVerbatim = false;
            bool bInterpolated = false;
            size_t Quote = i;
            while (Quote < Src.size() && (Src[Quote] == '@' || Src[Quote] == '$') && Quote - i < 2)
            {
                bVerbatim |= Src[Quote] == '@';
                bInterpolated |= Src[Quote] == '$';
                ++Quote;
            }

            if (Quote < Src.size() && Src[Quote] == '"')
            {
                FString Literal;
                bool bCut = false;
                i = ReadLiteral(Src, Quote, bVerbatim, bInterpolated, Literal, bCut);
                if (LooksLikeContentPath(FStringView(Literal.c_str(), Literal.size())))
                {
                    Out.emplace_back(Move(Literal));
                }
                continue;
            }

            ++i;
        }
        return Out;
    }

    FCSharpAssetScan::FResult FCSharpAssetScan::ScanRoots(const TVector<FString>& VirtualRoots, const FAssetRegistry& Registry,
                                                           const TFunction<void(FStringView)>& LogFunc)
    {
        FResult Result;
        THashSet<FString> Seen;

        TVector<FString> Sources;
        for (const FString& Root : VirtualRoots)
        {
            VFS::RecursiveDirectoryIterator(Root, [&](const VFS::FFileInfo& Info)
            {
                if (!Info.IsDirectory() && IsScannedSource(FStringView(Info.VirtualPath.c_str(), Info.VirtualPath.size())))
                {
                    Sources.emplace_back(Info.VirtualPath.c_str(), Info.VirtualPath.size());
                }
            });
        }

        for (const FString& Source : Sources)
        {
            TVector<uint8> Bytes;
            if (!VFS::ReadFile(Bytes, FStringView(Source.c_str(), Source.size())))
            {
                continue;
            }
            ++Result.FilesScanned;

            // A data file can name thousands of assets, so only code gets a line per reference.
            const bool bLogEachRef = IsCSharpSource(FStringView(Source.c_str(), Source.size()));
            const size_t AssetsBefore = Result.AssetPaths.size();
            const size_t FoldersBefore = Result.FolderPaths.size();

            const FStringView Contents(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
            for (FString& Candidate : ExtractCandidates(Contents))
            {
                ++Result.RawCandidates;
                if (!Seen.insert(Candidate).second)
                {
                    continue;
                }

                FString AssetPath = Candidate;
                if (!AssetPath.ends_with(".lasset") && !AssetPath.ends_with("/"))
                {
                    AssetPath += ".lasset";
                }

                const char* Kind = nullptr;
                if (Registry.GetAssetByPath(FStringView(Candidate.c_str(), Candidate.size())) != nullptr)
                {
                    Result.AssetPaths.push_back(Candidate);
                    Kind = "asset";
                }
                else if (AssetPath != Candidate && Registry.GetAssetByPath(FStringView(AssetPath.c_str(), AssetPath.size())) != nullptr)
                {
                    Result.AssetPaths.push_back(AssetPath);
                    Kind = "asset";
                }
                else if (IsSpecificFolder(FStringView(Candidate.c_str(), Candidate.size())) && VFS::IsDirectory(FStringView(Candidate.c_str(), Candidate.size())))
                {
                    Result.FolderPaths.push_back(Candidate);
                    Kind = "folder";
                }

                if (Kind != nullptr && LogFunc && bLogEachRef)
                {
                    FString Msg = FString("  [cs-ref] ") + Kind + " " + Candidate + "  (from " + Source + ")";
                    LogFunc(FStringView(Msg.c_str(), Msg.size()));
                }
            }

            const size_t NewAssets = Result.AssetPaths.size() - AssetsBefore;
            const size_t NewFolders = Result.FolderPaths.size() - FoldersBefore;
            if (!bLogEachRef && LogFunc && (NewAssets > 0 || NewFolders > 0))
            {
                const FString Msg = Format("  [data-ref] {} names {} asset(s) and {} folder(s)", Source.c_str(), NewAssets, NewFolders);
                LogFunc(FStringView(Msg.c_str(), Msg.size()));
            }
        }
        return Result;
    }
}
