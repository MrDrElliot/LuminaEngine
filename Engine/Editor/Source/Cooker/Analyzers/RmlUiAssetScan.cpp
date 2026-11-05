#include "RmlUiAssetScan.h"

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

#include <cctype>


namespace Lumina
{
    namespace
    {
        bool IsPathChar(char c)
        {
            // A conservative VFS path set, rejecting quotes, whitespace, parens and attribute delimiters.
            return c != '\0'
                && !std::isspace(static_cast<unsigned char>(c))
                && c != '"' && c != '\''
                && c != '(' && c != ')'
                && c != '<' && c != '>'
                && c != ';' && c != ',';
        }

        // Blank out `<!-- -->` and `/* */` comment ranges with equal-length spaces, preserving byte offsets and keeping downstream scanners comment-agnostic.
        FString StripCommentRanges(FStringView Src)
        {
            FString Out(Src.GetData(), Src.Len());
            auto Blank = [&](int32 From, int32 To)
            {
                for (int32 i = From; i < To && i < Out.Len(); ++i)
                {
                    if (Out[i] != '\n') Out[i] = ' ';
                }
            };

            int32 i = 0;
            while (i < Out.Len())
            {
                // HTML/RML comment.
                if (i + 3 < Out.Len()
                    && Out[i] == '<' && Out[i + 1] == '!'
                    && Out[i + 2] == '-' && Out[i + 3] == '-')
                {
                    const int32 Start = i;
                    i += 4;
                    while (i + 2 < Out.Len()
                        && !(Out[i] == '-' && Out[i + 1] == '-' && Out[i + 2] == '>'))
                    {
                        ++i;
                    }
                    const int32 End = (i + 2 < Out.Len()) ? (i + 3) : Out.Len();
                    Blank(Start, End);
                    i = End;
                    continue;
                }
                // CSS/RCSS block comment.
                if (i + 1 < Out.Len() && Out[i] == '/' && Out[i + 1] == '*')
                {
                    const int32 Start = i;
                    i += 2;
                    while (i + 1 < Out.Len() && !(Out[i] == '*' && Out[i + 1] == '/'))
                    {
                        ++i;
                    }
                    const int32 End = (i + 1 < Out.Len()) ? (i + 2) : Out.Len();
                    Blank(Start, End);
                    i = End;
                    continue;
                }
                ++i;
            }
            return Out;
        }

        // Rejects a match that is really the tail of another identifier, such as data-src or nosrc.
        bool IsAttributeBoundary(FStringView Src, int32 Hit)
        {
            if (Hit == 0) return true;
            const char Prev = Src[Hit - 1];
            return !(std::isalnum(static_cast<unsigned char>(Prev))
                || Prev == '_' || Prev == '-');
        }

        // Scans forward for a path-like substring starting with a slash and naming something below a mount.
        FStringView ScanPath(FStringView Src, int32 Pos)
        {
            // Skip leading whitespace and a single optional quote.
            while (Pos < Src.Len() && std::isspace(static_cast<unsigned char>(Src[Pos]))) ++Pos;
            if (Pos < Src.Len() && (Src[Pos] == '"' || Src[Pos] == '\'')) ++Pos;

            if (Pos >= Src.Len() || Src[Pos] != '/')
            {
                return {};
            }

            const int32 Start = Pos;
            while (Pos < Src.Len() && IsPathChar(Src[Pos]))
            {
                ++Pos;
            }
            FStringView Candidate = Src.Mid(Start, Pos - Start);

            // An asset path may omit .lasset, as a spritesheet src does, so only a bare mount such as /Game is rejected here.
            if (Candidate.Find('/', 1) == Constants::kIndexNone)
            {
                return {};
            }
            return Candidate;
        }

        // Capture the path after each `<Key>=`; left-anchored against an identifier-tail so `data-src=`/`nosrc=` don't false-match. Tolerates whitespace around `=`.
        void ScanAttribute(FStringView Src, FStringView Key, TVector<FString>& Out)
        {
            int32 Pos = 0;
            while (Pos < Src.Len())
            {
                int32 Hit = Src.Find(Key, Pos);
                if (Hit == Constants::kIndexNone) return;
                Pos = Hit + Key.Len();

                if (!IsAttributeBoundary(Src, Hit)) continue;

                int32 P = Pos;
                while (P < Src.Len() && std::isspace(static_cast<unsigned char>(Src[P]))) ++P;
                if (P >= Src.Len() || Src[P] != '=') continue;
                ++P;

                FStringView Path = ScanPath(Src, P);
                if (!Path.IsEmpty())
                {
                    Out.Emplace(Path.GetData(), Path.Len());
                    Pos = P + Path.Len();
                }
            }
        }

        // A declaration such as the src of an @spritesheet, which names its texture without url().
        void ScanCssProperty(FStringView Src, FStringView Key, TVector<FString>& Out)
        {
            int32 Pos = 0;
            while (Pos < Src.Len())
            {
                int32 Hit = Src.Find(Key, Pos);
                if (Hit == Constants::kIndexNone) return;
                Pos = Hit + Key.Len();

                if (!IsAttributeBoundary(Src, Hit)) continue;

                int32 P = Pos;
                while (P < Src.Len() && std::isspace(static_cast<unsigned char>(Src[P]))) ++P;
                if (P >= Src.Len() || Src[P] != ':') continue;
                ++P;

                FStringView Path = ScanPath(Src, P);
                if (!Path.IsEmpty())
                {
                    Out.Emplace(Path.GetData(), Path.Len());
                    Pos = P + Path.Len();
                }
            }
        }

        // A CSS function such as url( or RmlUi's image( decorator, in single, double and unquoted forms, boundary-checked against ident tails.
        void ScanCssFunction(FStringView Src, FStringView Tok, TVector<FString>& Out)
        {
            int32 Pos = 0;
            while (Pos < Src.Len())
            {
                int32 Hit = Src.Find(Tok, Pos);
                if (Hit == Constants::kIndexNone) return;
                Pos = Hit + Tok.Len();

                if (!IsAttributeBoundary(Src, Hit)) continue;

                FStringView Path = ScanPath(Src, Pos);
                if (!Path.IsEmpty())
                {
                    Out.Emplace(Path.GetData(), Path.Len());
                    Pos += Path.Len();
                }
            }
        }

        // The UI material brush URI scheme, boundary-checked so ordinary prose cannot match it.
        void ScanMaterialUri(FStringView Src, TVector<FString>& Out)
        {
            const FStringView Tok("material:");
            int32 Pos = 0;
            while (Pos < Src.Len())
            {
                int32 Hit = Src.Find(Tok, Pos);
                if (Hit == Constants::kIndexNone) return;
                Pos = Hit + Tok.Len();

                if (!IsAttributeBoundary(Src, Hit)) continue;

                FStringView Path = ScanPath(Src, Pos);
                if (!Path.IsEmpty())
                {
                    Out.Emplace(Path.GetData(), Path.Len());
                    Pos += Path.Len();
                }
            }
        }

        bool IsRmlOrRcss(FStringView VirtualPath)
        {
            FStringView Ext = VFS::Extension(VirtualPath);
            return Ext == ".rml" || Ext == ".rcss";
        }
    }


    TVector<FString> FRmlUiAssetScan::ExtractCandidates(FStringView Contents)
    {
        // Stripped first so commented-out examples never show up as cook roots.
        const FString Stripped = StripCommentRanges(Contents);
        const FStringView Src(*Stripped, Stripped.Len());

        TVector<FString> Out;
        Out.Reserve(8);

        // RML/HTML attributes that commonly carry asset paths.
        ScanAttribute(Src, FStringView("src"),        Out);
        ScanAttribute(Src, FStringView("href"),       Out);
        ScanAttribute(Src, FStringView("sprite"),     Out);
        ScanAttribute(Src, FStringView("data-asset"), Out);

        // CSS / RCSS.
        ScanCssProperty(Src, FStringView("src"), Out);
        ScanCssFunction(Src, FStringView("url("), Out);
        ScanCssFunction(Src, FStringView("image("), Out);
        ScanMaterialUri(Src, Out);

        return Out;
    }


    FRmlUiAssetScan::FResult FRmlUiAssetScan::ScanRoots(
        const TVector<FString>& VirtualRoots,
        const FAssetRegistry& Registry,
        const TFunction<void(FStringView)>& LogFunc)
    {
        FResult Result;
        THashSet<FString> UniqueAssets;
        THashSet<FString> VisitedDocs;
        TVector<FString>  Worklist;

        // Seeded from every .rml and .rcss under the content roots, then @import chains are followed.
        for (const FString& Root : VirtualRoots)
        {
            VFS::RecursiveDirectoryIterator(Root, [&](const VFS::FFileInfo& Info)
            {
                if (Info.IsDirectory()) return;
                FStringView Vp(*Info.VirtualPath, Info.VirtualPath.Len());
                if (!IsRmlOrRcss(Vp)) return;
                Worklist.Emplace(Vp.GetData(), Vp.Len());
            });
        }

        while (!Worklist.IsEmpty())
        {
            FString DocPath = Move(Worklist.Last());
            Worklist.Pop();

            if (VisitedDocs.Contains(DocPath)) continue;
            VisitedDocs.Add(DocPath);

            FStringView Vp(*DocPath, DocPath.Len());

            TVector<uint8> Bytes;
            if (!VFS::ReadFile(Bytes, Vp)) continue;

            ++Result.FilesScanned;

            FStringView Contents(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num());
            TVector<FString> Candidates = ExtractCandidates(Contents);
            Result.RawCandidates += Candidates.Num();

            for (FString& Candidate : Candidates)
            {
                FStringView CandView(*Candidate, Candidate.Len());

                // Another .rml or .rcss on disk is queued for its own scan, with no registry membership required.
                if (IsRmlOrRcss(CandView) && VFS::Exists(Candidate)
                    && !VisitedDocs.Contains(Candidate))
                {
                    Worklist.Emplace(Candidate);
                }

                if (UniqueAssets.Contains(Candidate))
                {
                    continue;
                }
                // Only registered-asset paths count as cook roots; .ttf/.png loose files still ship via BundleLooseContent.
                if (Registry.GetAssetByPath(CandView) == nullptr)
                {
                    FString WithExtension = Candidate + ".lasset";
                    if (VFS::Extension(CandView).IsEmpty()
                        && Registry.GetAssetByPath(FStringView(*WithExtension, WithExtension.Len())) != nullptr)
                    {
                        Candidate = Move(WithExtension);
                    }
                    else
                    {
                        continue;
                    }
                    if (UniqueAssets.Contains(Candidate))
                    {
                        continue;
                    }
                }

                if (LogFunc)
                {
                    FString Msg("  [rml-ref] ");
                    Msg += Candidate;
                    Msg += "  (from ";
                    Msg.Append(Vp.GetData(), Vp.Len());
                    Msg += ")";
                    LogFunc(FStringView(*Msg, Msg.Len()));
                }
                UniqueAssets.Add(Candidate);
                Result.AssetPaths.Emplace(Move(Candidate));
                ++Result.ResolvedRefs;
            }
        }

        return Result;
    }
}
