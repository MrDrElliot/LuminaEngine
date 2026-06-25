#include "ContentBrowserEditorTool.h"

#include "EditorToolContext.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetRegistry/TextAssetTypes.h"
#include "Assets/AssetRegistry/TextAssetSidecar.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/Package/Package.h"
#include "EASTL/sort.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/ThreadedCallback.h"
#include "Tools/Dialogs/Dialogs.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "World/WorldManager.h"
#include <string.h>
#include <cstdarg>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <Assets/AssetRegistry/AssetData.h>
#include <Containers/Array.h>
#include <Containers/Function.h>
#include <Containers/String.h>
#include <Core/LuminaCommonTypes.h>
#include <Core/Math/Hash/Hash.h>
#include <Core/Object/Class.h>
#include <Core/Object/Object.h>
#include <Core/Object/ObjectCore.h>
#include <Core/Templates/LuminaTemplate.h>
#include <Events/Event.h>
#include <FileSystem/FileInfo.h>
#include <Memory/SmartPtr.h>
#include <Core/Plugin/Plugin.h>
#include <Core/Plugin/PluginManager.h>
#include <Memory/SmartPtr.h>
#include <Platform/Filesystem/DirectoryWatcher.h>
#include <Platform/GenericPlatform.h>
#include <Platform/Platform.h>
#include <Tools/UI/ImGui/ImGuiDesignIcons.h>
#include <Tools/UI/ImGui/Widgets/TileViewWidget.h>
#include <Tools/UI/ImGui/Widgets/TreeListView.h>
#include <imgui.h>
#include <imgui_internal.h>
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Thumbnails/ThumbnailManager.h"
#include <LuminaEditor.h>
#include "Scripting/DotNet/DotNetHost.h"
#include <atomic>

#include "Config/Config.h"
#include "Core/Object/ObjectCore.h"
#include "Settings/EditorSettings.h"
#include "Tools/Import/ImportHelpers.h"

namespace Lumina
{
    namespace
    {

        constexpr ImVec4 kMenuBg            = ImVec4(0.10f, 0.10f, 0.12f, 0.98f);
        constexpr ImVec4 kMenuBorder        = ImVec4(0.22f, 0.23f, 0.27f, 1.00f);
        constexpr ImVec4 kMenuText          = ImVec4(0.90f, 0.90f, 0.93f, 1.00f);
        constexpr ImVec4 kMenuTextDim       = ImVec4(0.55f, 0.56f, 0.62f, 1.00f);
        constexpr ImVec4 kMenuTextSection   = ImVec4(0.50f, 0.58f, 0.72f, 1.00f);
        constexpr ImVec4 kMenuAccent        = ImVec4(0.36f, 0.66f, 1.00f, 1.00f);
        constexpr ImVec4 kMenuAccentFolder  = ImVec4(1.00f, 0.78f, 0.40f, 1.00f);
        constexpr ImVec4 kMenuAccentScript  = ImVec4(0.52f, 0.85f, 0.55f, 1.00f);
        constexpr ImVec4 kMenuDanger        = ImVec4(0.96f, 0.36f, 0.38f, 1.00f);
        constexpr ImVec4 kMenuDangerHover   = ImVec4(0.85f, 0.22f, 0.24f, 0.45f);
        constexpr ImVec4 kMenuHeaderHover   = ImVec4(0.24f, 0.46f, 0.78f, 0.55f);
        constexpr ImVec4 kMenuHeader        = ImVec4(0.24f, 0.46f, 0.78f, 0.30f);
        constexpr ImVec4 kMenuHeaderActive  = ImVec4(0.24f, 0.46f, 0.78f, 0.85f);
        constexpr ImVec4 kMenuSeparator     = ImVec4(0.24f, 0.25f, 0.30f, 0.65f);
        constexpr ImVec4 kMenuHeaderBg      = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);

        // Popup-window-level styles. Push BEFORE BeginPopup* so the popup window picks them up.
        void PushContextMenuWindowStyle()
        {
            ImGui::PushStyleColor(ImGuiCol_PopupBg, kMenuBg);
            ImGui::PushStyleColor(ImGuiCol_Border,  kMenuBorder);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,   8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2(6.0f, 6.0f));
        }

        void PopContextMenuWindowStyle()
        {
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);
        }

        // Per-item styles. Push inside BeginPopup..EndPopup for consistent menu items.
        void PushContextMenuItemStyle()
        {
            ImGui::PushStyleColor(ImGuiCol_Text,          kMenuText);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kMenuHeaderHover);
            ImGui::PushStyleColor(ImGuiCol_Header,        kMenuHeader);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive,  kMenuHeaderActive);
            ImGui::PushStyleColor(ImGuiCol_Separator,     kMenuSeparator);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,    4.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(8.0f, 3.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,     ImVec2(10.0f, 5.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(8.0f, 4.0f));
        }

        void PopContextMenuItemStyle()
        {
            ImGui::PopStyleVar(4);
            ImGui::PopStyleColor(5);
        }

        void DrawMenuSection(const char* Label)
        {
            ImGui::Spacing();
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::TinyBold);
            ImGui::PushStyleColor(ImGuiCol_Text, kMenuTextSection);
            const float OldX = ImGui::GetCursorPosX();
            ImGui::SetCursorPosX(OldX + 4.0f);
            ImGui::TextUnformatted(Label);
            ImGui::PopStyleColor();
            ImGuiX::Font::PopFont();
        }

        void DrawMenuHeader(const char* Icon, const char* TitleStr, const char* SubtitleStr, const ImVec4& IconColor)
        {
            const bool bHasSubtitle = SubtitleStr && *SubtitleStr;

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::SmallBold);
            const float TitleH = ImGui::GetTextLineHeight();
            ImGuiX::Font::PopFont();

            float SubH = 0.0f;
            if (bHasSubtitle)
            {
                ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Tiny);
                SubH = ImGui::GetTextLineHeight();
                ImGuiX::Font::PopFont();
            }

            constexpr float TopPad   = 3.0f;
            constexpr float BotPad   = 3.0f;
            const     float Gap      = bHasSubtitle ? 0.0f : 0.0f;
            const     float HeaderH  = TopPad + TitleH + Gap + SubH + BotPad;

            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            const float  Avail = ImGui::GetContentRegionAvail().x;
            const ImVec2 P0    = ImGui::GetCursorScreenPos();
            const ImVec2 P1    = ImVec2(P0.x + Avail, P0.y + HeaderH);

            DrawList->AddRectFilled(P0, P1, ImGui::ColorConvertFloat4ToU32(kMenuHeaderBg), 4.0f);
            DrawList->AddRectFilled(P0, ImVec2(P0.x + 3.0f, P1.y), ImGui::ColorConvertFloat4ToU32(IconColor), 4.0f);

            ImGui::SetCursorScreenPos(ImVec2(P0.x + 9.0f, P0.y + TopPad));
            ImGui::PushStyleColor(ImGuiCol_Text, IconColor);
            ImGui::TextUnformatted(Icon);
            ImGui::PopStyleColor();

            ImGui::SameLine(0, 6.0f);
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::SmallBold);
            ImGui::PushStyleColor(ImGuiCol_Text, kMenuText);
            ImGui::TextUnformatted(TitleStr);
            ImGui::PopStyleColor();
            ImGuiX::Font::PopFont();

            if (bHasSubtitle)
            {
                ImGui::SetCursorScreenPos(ImVec2(P0.x + 9.0f, P0.y + TopPad + TitleH));
                ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Tiny);
                ImGui::PushStyleColor(ImGuiCol_Text, kMenuTextDim);
                ImGui::TextUnformatted(SubtitleStr);
                ImGui::PopStyleColor();
                ImGuiX::Font::PopFont();
            }

            ImGui::SetCursorScreenPos(ImVec2(P0.x, P1.y));
            ImGui::Dummy(ImVec2(Avail, 1.0f));
        }

        // Case-insensitive compare of a view against a NUL-terminated literal.
        bool IEquals(FStringView A, const char* B)
        {
            size_t i = 0;
            for (; i < A.size() && B[i] != '\0'; ++i)
            {
                char a = A[i]; if (a >= 'A' && a <= 'Z')
                {
                    a += 32;
                }
                char b = B[i]; if (b >= 'A' && b <= 'Z')
                {
                    b += 32;
                }
                if (a != b)
                {
                    return false;
                }
            }
            return i == A.size() && B[i] == '\0';
        }

        // Loose files the browser surfaces. Anything outside this set (generated .csproj, .lmeta
        // sidecars, IDE files, C# build output) is hidden so the grid shows only engine content.
        bool IsBrowsableFileExtension(FStringView Ext)
        {
            static constexpr const char* kSupported[] =
            {
                ".lasset", ".cs", ".rml", ".rcss",
            };
            for (const char* S : kSupported)
            {
                if (IEquals(Ext, S)) { return true; }
            }
            return false;
        }

        // Build / IDE directories that never belong in the browser (regenerated on demand).
        bool IsHiddenBrowserDirectory(FStringView Name)
        {
            static constexpr const char* kHidden[] = { "bin", "obj", ".vs", ".vscode", ".idea", ".git" };
            for (const char* S : kHidden)
            {
                if (IEquals(Name, S)) { return true; }
            }
            return false;
        }

        bool ShouldHideDirectory(const VFS::FFileInfo& Info)
        {
            if (Info.IsHidden()) { return true; }
            const FStringView Name(Info.Name.c_str(), Info.Name.size());
            if (!Name.empty() && Name.front() == '.') { return true; }
            if (IsHiddenBrowserDirectory(Name)) { return true; }
            
            const FStringView Parent = VFS::Parent(FStringView(Info.VirtualPath.c_str(), Info.VirtualPath.size()), true);
            if (IEquals(Parent, "/Engine/Resources") && !IEquals(Name, "Content") && !IEquals(Name, "Scripts"))
            {
                return true;
            }
            return false;
        }

        // Engine-managed root mounts shown as protected, undeletable folders: each project root (Game,
        // Engine) and its core Content + Scripts subdirs.
        bool IsProtectedRoot(FStringView VirtualPath)
        {
            return IEquals(VirtualPath, "/Game")
                || IEquals(VirtualPath, "/Game/Content")
                || IEquals(VirtualPath, "/Game/Scripts")
                || IEquals(VirtualPath, "/Engine/Resources")
                || IEquals(VirtualPath, "/Engine/Resources/Content")
                || IEquals(VirtualPath, "/Engine/Resources/Scripts");
        }

        // True if VirtualPath is a mount's "Scripts" subdir or anything beneath it.
        bool IsScriptDirectory(FStringView VirtualPath)
        {
            size_t Pos = 0;
            auto NextSegment = [&]() -> FStringView
            {
                while (Pos < VirtualPath.size() && VirtualPath[Pos] == '/') { ++Pos; }
                const size_t Begin = Pos;
                while (Pos < VirtualPath.size() && VirtualPath[Pos] != '/') { ++Pos; }
                return FStringView(VirtualPath.data() + Begin, Pos - Begin);
            };

            const FStringView Root = NextSegment();
            if (Root.empty())
            {
                return false;
            }
            if (IEquals(Root, "Engine"))
            {
                return IEquals(NextSegment(), "Resources") && IEquals(NextSegment(), "Scripts");
            }
            return IEquals(NextSegment(), "Scripts");
        }
        
        struct FScriptHoverInfo
        {
            bool             bValid = false;
            FString          Namespace;
            FString          ClassName;
            FString          BaseClass;
            FString          Summary;
            TVector<FString> Lifecycle;
            TVector<FString> Properties;
            TVector<FString> PropertyTips;
        };

        struct FScriptHoverCacheEntry
        {
            FScriptHoverCacheEntry() noexcept = default;
            
            FString          Path;
            int64            MTime = 0;
            FScriptHoverInfo Info;
        };

        FScriptHoverCacheEntry GScriptHoverCache;

        bool IsCsIdentChar(char C)
        {
            return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_';
        }

        // Strips C# access/storage modifiers off a field/property declaration, leaving "Type Name [= default]".
        FString CleanScriptDeclaration(const char* Begin, const char* End)
        {
            static const char* const Modifiers[] =
            {
                "public", "private", "protected", "internal", "static", "readonly",
                "const", "sealed", "virtual", "override", "new", "required", "volatile", "partial",
            };
            FString Out;
            const char* P = Begin;
            while (P < End)
            {
                while (P < End && (*P == ' ' || *P == '\t' || *P == '\r' || *P == '\n')) { ++P; }
                const char* TokStart = P;
                while (P < End && !(*P == ' ' || *P == '\t' || *P == '\r' || *P == '\n')) { ++P; }
                const size_t Len = (size_t)(P - TokStart);
                if (Len == 0)
                {
                    break;
                }
                bool bIsModifier = false;
                for (const char* Mod : Modifiers)
                {
                    if (Len == strlen(Mod) && memcmp(TokStart, Mod, Len) == 0) { bIsModifier = true; break; }
                }
                if (bIsModifier)
                {
                    continue;
                }
                if (!Out.empty())
                {
                    Out += ' ';
                }
                Out.append(TokStart, Len);
            }
            return Out;
        }

        FScriptHoverInfo ParseScriptHoverInfo(FStringView VirtualPath)
        {
            FScriptHoverInfo Info;
            FString Text;
            if (!VFS::ReadFile(Text, VirtualPath) || Text.empty())
            {
                return Info;
            }
            Info.bValid = true;
            const size_t N = Text.size();

            // namespace X
            {
                const size_t Pos = Text.find("namespace ");
                if (Pos != FString::npos)
                {
                    size_t P = Pos + 10;
                    while (P < N && (Text[P] == ' ' || Text[P] == '\t')) { ++P; }
                    const size_t Start = P;
                    while (P < N && (IsCsIdentChar(Text[P]) || Text[P] == '.')) { ++P; }
                    Info.Namespace.assign(Text.data() + Start, P - Start);
                }
            }

            // primary class declaration + base type
            size_t ClassPos = FString::npos;
            {
                size_t Search = 0;
                while (true)
                {
                    const size_t C = Text.find("class ", Search);
                    if (C == FString::npos) { break; }
                    if (C == 0 || !IsCsIdentChar(Text[C - 1])) { ClassPos = C; break; }
                    Search = C + 6;
                }
                if (ClassPos != FString::npos)
                {
                    size_t P = ClassPos + 6;
                    while (P < N && (Text[P] == ' ' || Text[P] == '\t')) { ++P; }
                    const size_t Start = P;
                    while (P < N && IsCsIdentChar(Text[P])) { ++P; }
                    Info.ClassName.assign(Text.data() + Start, P - Start);
                    while (P < N && (Text[P] == ' ' || Text[P] == '\t')) { ++P; }
                    if (P < N && Text[P] == ':')
                    {
                        ++P;
                        while (P < N && (Text[P] == ' ' || Text[P] == '\t')) { ++P; }
                        const size_t BStart = P;
                        while (P < N && (IsCsIdentChar(Text[P]) || Text[P] == '.')) { ++P; }
                        Info.BaseClass.assign(Text.data() + BStart, P - BStart);
                    }
                }
            }

            // /// <summary> doc comment
            {
                const size_t S = Text.find("<summary>");
                const size_t E = Text.find("</summary>");
                if (S != FString::npos && E != FString::npos && E > S && (ClassPos == FString::npos || S < ClassPos))
                {
                    FString Clean;
                    bool bPrevSpace = true;
                    for (size_t i = S + 9; i < E; ++i)
                    {
                        const char Ch = Text[i];
                        if (Ch == '/' || Ch == '\r') { continue; }
                        if (Ch == '\n' || Ch == '\t' || Ch == ' ')
                        {
                            if (!bPrevSpace) { Clean += ' '; bPrevSpace = true; }
                            continue;
                        }
                        Clean += Ch;
                        bPrevSpace = false;
                    }
                    while (!Clean.empty() && Clean.back() == ' ') { Clean.pop_back(); }
                    Info.Summary = Clean;
                }
            }

            // overridden methods (lifecycle hooks: OnReady/OnUpdate/OnInput/...)
            {
                size_t P = 0;
                while (Info.Lifecycle.size() < 16)
                {
                    const size_t O = Text.find("override", P);
                    if (O == FString::npos) { break; }
                    P = O + 8;
                    if ((O > 0 && IsCsIdentChar(Text[O - 1])) || (P < N && IsCsIdentChar(Text[P]))) { continue; }
                    FString Method;
                    size_t Q = P;
                    while (Q < N && Text[Q] != '\n' && Text[Q] != '{' && Text[Q] != ';')
                    {
                        while (Q < N && !IsCsIdentChar(Text[Q]) && Text[Q] != '\n' && Text[Q] != '{' && Text[Q] != ';') { ++Q; }
                        const size_t Start = Q;
                        while (Q < N && IsCsIdentChar(Text[Q])) { ++Q; }
                        if (Q > Start)
                        {
                            size_t R = Q;
                            while (R < N && (Text[R] == ' ' || Text[R] == '\t')) { ++R; }
                            if (R < N && Text[R] == '(') { Method.assign(Text.data() + Start, Q - Start); break; }
                        }
                        else
                        {
                            ++Q;
                        }
                    }
                    if (!Method.empty())
                    {
                        bool bDup = false;
                        for (const FString& M : Info.Lifecycle) { if (M == Method) { bDup = true; break; } }
                        if (!bDup) { Info.Lifecycle.push_back(Method); }
                    }
                }
            }

            // [Property] exported fields (+ Tooltip="...")
            {
                size_t P = 0;
                while (Info.Properties.size() < 24)
                {
                    const size_t A = Text.find("[Property", P);
                    if (A == FString::npos) { break; }
                    const size_t Brk = Text.find(']', A);
                    if (Brk == FString::npos) { break; }
                    P = Brk + 1;

                    FString Tip;
                    {
                        const size_t T = Text.find("Tooltip", A);
                        if (T != FString::npos && T < Brk)
                        {
                            const size_t Q1 = Text.find('"', T);
                            if (Q1 != FString::npos && Q1 < Brk)
                            {
                                const size_t Q2 = Text.find('"', Q1 + 1);
                                if (Q2 != FString::npos) { Tip.assign(Text.data() + Q1 + 1, Q2 - Q1 - 1); }
                            }
                        }
                    }

                    // Skip past any further attribute lines to the actual declaration.
                    size_t D = Brk + 1;
                    for (;;)
                    {
                        while (D < N && (Text[D] == ' ' || Text[D] == '\t' || Text[D] == '\r' || Text[D] == '\n')) { ++D; }
                        if (D < N && Text[D] == '[')
                        {
                            const size_t E = Text.find(']', D);
                            if (E == FString::npos) { D = N; break; }
                            D = E + 1;
                            continue;
                        }
                        break;
                    }
                    size_t End = D;
                    while (End < N && Text[End] != ';' && Text[End] != '{' && Text[End] != '\n') { ++End; }
                    FString Decl = CleanScriptDeclaration(Text.data() + D, Text.data() + End);
                    if (!Decl.empty())
                    {
                        Info.Properties.push_back(Decl);
                        Info.PropertyTips.push_back(Tip);
                    }
                }
            }

            return Info;
        }

        void UpdateScriptHoverCache(FStringView VirtualPath, FStringView DiskPath)
        {
            int64 MTime = 0;
            std::error_code Ec;
            const std::filesystem::file_time_type Time = std::filesystem::last_write_time(std::filesystem::path(DiskPath.data(), DiskPath.data() + DiskPath.size()), Ec);
            if (!Ec) { MTime = (int64)Time.time_since_epoch().count(); }

            const bool bSamePath = GScriptHoverCache.Path.size() == VirtualPath.size()
                && memcmp(GScriptHoverCache.Path.data(), VirtualPath.data(), VirtualPath.size()) == 0;
            if (bSamePath && GScriptHoverCache.MTime == MTime && GScriptHoverCache.Info.bValid)
            {
                return;
            }
            GScriptHoverCache.Path.assign(VirtualPath.data(), VirtualPath.size());
            GScriptHoverCache.MTime = MTime;
            GScriptHoverCache.Info = ParseScriptHoverInfo(VirtualPath);
        }

        void DrawScriptHoverContent(const FScriptHoverInfo& Info)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kMenuAccentScript);
            ImGui::TextUnformatted(Info.ClassName.empty() ? "C# Script" : Info.ClassName.c_str());
            ImGui::PopStyleColor();
            if (!Info.BaseClass.empty())
            {
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::TextColored(kMenuTextDim, ": %s", Info.BaseClass.c_str());
            }
            if (!Info.Namespace.empty())
            {
                ImGui::TextColored(kMenuTextDim, "namespace %s", Info.Namespace.c_str());
            }

            if (!Info.Summary.empty())
            {
                ImGui::Spacing();
                ImGui::TextUnformatted(Info.Summary.c_str());
            }

            if (!Info.Lifecycle.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(kMenuTextSection, "Lifecycle");
                FString Line;
                for (size_t i = 0; i < Info.Lifecycle.size(); ++i)
                {
                    if (i != 0) { Line += ", "; }
                    Line += Info.Lifecycle[i];
                }
                ImGui::TextUnformatted(Line.c_str());
            }

            if (!Info.Properties.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(kMenuTextSection, "Properties");
                const size_t Max = 12;
                for (size_t i = 0; i < Info.Properties.size() && i < Max; ++i)
                {
                    ImGui::BulletText("%s", Info.Properties[i].c_str());
                    if (i < Info.PropertyTips.size() && !Info.PropertyTips[i].empty())
                    {
                        ImGui::Indent(14.0f);
                        ImGui::TextColored(kMenuTextDim, "%s", Info.PropertyTips[i].c_str());
                        ImGui::Unindent(14.0f);
                    }
                }
                if (Info.Properties.size() > Max)
                {
                    ImGui::TextColored(kMenuTextDim, "(+%d more)", (int)(Info.Properties.size() - Max));
                }
            }

            if (Info.Summary.empty() && Info.Lifecycle.empty() && Info.Properties.empty())
            {
                ImGui::TextColored(kMenuTextDim, "C# script source");
            }
        }

        // File size line (from the on-disk source) shared by the non-script tooltip kinds.
        void DrawItemSizeLine(const VFS::FFileInfo& Info)
        {
            std::error_code Ec;
            const std::uintmax_t Bytes = std::filesystem::file_size(std::filesystem::path(Info.PathSource.c_str()), Ec);
            if (Ec) { return; }
            const double B = (double)Bytes;
            if (Bytes < 1024ull)                    { ImGui::TextColored(kMenuTextDim, "Size: %llu B", (unsigned long long)Bytes); }
            else if (Bytes < 1024ull * 1024)        { ImGui::TextColored(kMenuTextDim, "Size: %.1f KB", B / 1024.0); }
            else if (Bytes < 1024ull * 1024 * 1024) { ImGui::TextColored(kMenuTextDim, "Size: %.1f MB", B / (1024.0 * 1024.0)); }
            else                                    { ImGui::TextColored(kMenuTextDim, "Size: %.2f GB", B / (1024.0 * 1024.0 * 1024.0)); }
        }

        // Rich tooltip body for a .lasset: type, owning plugin, outbound refs, cook flags, size, GUID.
        void DrawAssetTooltipContent(const VFS::FFileInfo& Info)
        {
            const FStringView VPath(Info.VirtualPath.c_str(), Info.VirtualPath.size());
            const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(VPath);
            if (Data == nullptr)
            {
                ImGui::TextColored(kMenuTextDim, "Asset (not yet indexed)");
                DrawItemSizeLine(Info);
                return;
            }

            ImGui::PushStyleColor(ImGuiCol_Text, kMenuAccentScript);
            ImGui::TextUnformatted(Data->AssetClass.IsNone() ? "Asset" : Data->AssetClass.c_str());
            ImGui::PopStyleColor();

            if (!Data->OwningPlugin.IsNone())
            {
                ImGui::TextColored(kMenuTextDim, "Plugin: %s", Data->OwningPlugin.c_str());
            }
            if (!Data->Dependencies.empty())
            {
                ImGui::TextColored(kMenuTextDim, "References: %d", (int)Data->Dependencies.size());
            }

            FString Flags;
            const auto AddFlag = [&Flags](const char* Name)
            {
                if (!Flags.empty()) { Flags += ", "; }
                Flags += Name;
            };
            if (HasFlag(Data->Flags, EAssetFlags::EditorOnly))  { AddFlag("Editor-Only"); }
            if (HasFlag(Data->Flags, EAssetFlags::RuntimeOnly)) { AddFlag("Runtime-Only"); }
            if (HasFlag(Data->Flags, EAssetFlags::AlwaysCook))  { AddFlag("Always Cook"); }
            if (HasFlag(Data->Flags, EAssetFlags::NeverCook))   { AddFlag("Never Cook"); }
            if (HasFlag(Data->Flags, EAssetFlags::Primary))     { AddFlag("Primary"); }
            if (!Flags.empty())
            {
                ImGui::TextColored(kMenuTextDim, "Flags: %s", Flags.c_str());
            }

            DrawItemSizeLine(Info);
            ImGui::TextColored(kMenuTextDim, "GUID: %s", Data->AssetGUID.ToString(false, true).c_str());
        }

        std::atomic<bool> GScriptReloadQueued{ false };
    }

    // Single framework-driven hover tooltip (called via BeginItemTooltip). Rich, per-kind content:
    // scripts show class/lifecycle/properties; assets show type/refs/flags/size/GUID; other files show
    // type + size; folders show item counts.
    void FContentBrowserEditorTool::FContentBrowserTileViewItem::DrawTooltip() const
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, kMenuAccent);
        ImGui::TextUnformatted(FileInfo.Name.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();

        switch (IconKind)
        {
        case EIconKind::CSharpScript:
            {
                const FStringView VPath(FileInfo.VirtualPath.c_str(), FileInfo.VirtualPath.size());
                const FStringView DPath(FileInfo.PathSource.c_str(), FileInfo.PathSource.size());
                UpdateScriptHoverCache(VPath, DPath);
                DrawScriptHoverContent(GScriptHoverCache.Info);
                break;
            }
        case EIconKind::Asset:
            {
                DrawAssetTooltipContent(FileInfo);
                break;
            }
        case EIconKind::Directory:
            {
                int32 Count = 0;
                const FStringView VPath(FileInfo.VirtualPath.c_str(), FileInfo.VirtualPath.size());
                VFS::DirectoryIterator(VPath, [&Count](const VFS::FFileInfo&) { ++Count; });
                ImGui::TextColored(kMenuTextDim, "Folder - %d item%s", Count, Count == 1 ? "" : "s");
                break;
            }
        case EIconKind::Markup:
            {
                ImGui::TextColored(kMenuTextDim, "UI Document (.rml)");
                DrawItemSizeLine(FileInfo);
                break;
            }
        case EIconKind::Stylesheet:
            {
                ImGui::TextColored(kMenuTextDim, "UI Stylesheet (.rcss)");
                DrawItemSizeLine(FileInfo);
                break;
            }
        case EIconKind::Audio:
            {
                ImGui::TextColored(kMenuTextDim, "Audio Clip (%s)", FileInfo.GetExt().c_str());
                DrawItemSizeLine(FileInfo);
                break;
            }
        case EIconKind::Generic:
            {
                const FString Ext = FileInfo.GetExt();
                if (!Ext.empty())
                {
                    ImGui::TextColored(kMenuTextDim, "%s file", Ext.c_str());
                }
                DrawItemSizeLine(FileInfo);
                break;
            }
        }

        ImGui::Spacing();
        ImGui::TextColored(kMenuTextDim, "%s", FileInfo.VirtualPath.c_str());

        ImGui::PopTextWrapPos();
    }

    template<size_t BufferSize = 42>
    class FRenameModalState
    {
    public:
        
        void Initialize(FStringView CurrentName)
        {
            Buffer.assign(CurrentName.begin(), CurrentName.end());
        }

        
        // ReSharper disable once CppMemberFunctionMayBeStatic
        NODISCARD constexpr size_t Capacity() const { return BufferSize; }
        FORCEINLINE NODISCARD char* CStr() { return Buffer.data(); }
        FORCEINLINE NODISCARD bool IsValid() const { return !Buffer.empty(); }
        
    private:
        
        TFixedString<BufferSize> Buffer;
    };

    bool FContentBrowserEditorTool::OnEvent(FEvent& Event)
    {
        if (Event.IsA<FFileDropEvent>())
        {
            FFileDropEvent& FileEvent = Event.As<FFileDropEvent>();

            ImVec2 DropCursor = ImVec2(FileEvent.GetMouseX(), FileEvent.GetMouseY());

            for (const FFixedString& Path : FileEvent.GetPaths())
            {
                ActionRegistry.EnqueueAction<FPendingOSDrop>(FPendingOSDrop{ Path, DropCursor });
            }

            return true;
        }

        return false;
    }

    void FContentBrowserEditorTool::RefreshContentBrowser()
    {
        ContentBrowserTileView.MarkTreeDirty();
        DirectoryListView.MarkTreeDirty();
    }

    void FContentBrowserEditorTool::OnInitialize()
    {
        (void)FAssetRegistry::Get().GetOnAssetRegistryUpdated().AddMember(this, &FContentBrowserEditorTool::RefreshContentBrowser);
        (void)GEditorEngine->GetProjectLoadedDelegate().AddMember(this, &FContentBrowserEditorTool::OnProjectLoaded);
        
        ContentBrowserTileSize = GetDefault<CContentBrowserSettings>()->TileSize;
        ContentBrowserTileView.SetTileSize(ContentBrowserTileSize);

        if (GEditorEngine->HasLoadedProject())
        {
            // Virtual mount path, not the native content dir, the browser iterates VFS.
            SelectedPath = "/Game";
        }

        const TVector<CFactory*>& Factories = CFactoryRegistry::Get().GetFactories();
        for (CFactory* Factory : Factories)
        {
            if (CClass* AssetClass = Factory->GetAssetClass())
            {
                FilterState.emplace(AssetClass->GetName().c_str(), true);
            }
        }
        
        CreateToolWindow("Content", [&] (bool bIsFocused)
        {
            float Left = 225.0f;
            float Right = ImGui::GetContentRegionAvail().x - Left;
            
            DrawDirectoryBrowser(bIsFocused, ImVec2(Left, 0));
            
            ImGui::SameLine();

            DrawContentBrowser(bIsFocused, ImVec2(Right, 0));
        });
        
        ContentBrowserTileViewContext.DragDropFunction = [this] (FTileViewItem* DropItem, const TVector<FTileViewItem*>& Selections)
        {
            auto* TypedDroppedItem = static_cast<FContentBrowserTileViewItem*>(DropItem);
            if (!TypedDroppedItem->IsDirectory())
            {
                return;
            }

            const DragDrop::FPayload* Peek = DragDrop::PeekPayload();
            if (Peek == nullptr || !DragDrop::IsDelivered())
            {
                return;
            }

            FStringView SourcePath;
            if (Peek->Kind == DragDrop::EPayloadKind::Asset)
            {
                SourcePath = FStringView(Peek->AssetPath.c_str(), Peek->AssetPath.size());
            }
            else if (Peek->Kind == DragDrop::EPayloadKind::File)
            {
                SourcePath = FStringView(Peek->FilePath.c_str(), Peek->FilePath.size());
            }
            else
            {
                return;
            }

            if (SourcePath != TypedDroppedItem->GetVirtualPath())
            {
                HandleContentBrowserDragDrop(TypedDroppedItem->GetVirtualPath(), SourcePath);
            }

            for (FTileViewItem* Item : Selections)
            {
                auto* SourceItem = reinterpret_cast<FContentBrowserTileViewItem*>(Item);

                if (SourceItem->GetVirtualPath() == SourcePath)
                {
                    continue;
                }

                if (SourceItem == TypedDroppedItem)
                {
                    continue;
                }

                HandleContentBrowserDragDrop(TypedDroppedItem->GetVirtualPath(), SourceItem->GetVirtualPath());
            }
        };

        ContentBrowserTileViewContext.DrawItemOverrideFunction = [this] (FTileViewItem* Item)
        {
            FContentBrowserTileViewItem* ContentItem = static_cast<FContentBrowserTileViewItem*>(Item);
            
            ImVec4 TintColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

            ImTextureRef ImTexture;
            switch (ContentItem->GetIconKind())
            {
            case EIconKind::Directory:
                {
                    ImTexture = ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/Folder.png");
                    TintColor  = ImVec4(1.0f, 0.9f, 0.6f, 1.0f);
                    break;
                }
            case EIconKind::Asset:
                {
                    if (FPackageThumbnail* MaybeThumbnail = CThumbnailManager::Get().GetThumbnailForPackage(ContentItem->GetVirtualPath()))
                    {
                        ImTexture = ImGuiX::ToImTextureRef(MaybeThumbnail->LoadedImage);
                    }
                    else
                    {
                        ImTexture = ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/Asset.png");
                    }
                    break;
                }
            case EIconKind::CSharpScript:
                {
                    ImTexture = ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/CSharpScript.png");
                    break;
                }
            case EIconKind::Markup:
                {
                    ImTexture = ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/HTML.png");
                    break;
                }
            case EIconKind::Stylesheet:
                {
                    ImTexture = ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/CSS.png");
                    break;
                }
            case EIconKind::Audio:
                {
                    ImTexture = ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/Audio.png");
                    break;
                }
            case EIconKind::Generic:
                ImTexture = ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/File.png");
                break;
            }

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.16f, 0.17f, 1.0f)); 
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.22f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.26f, 0.26f, 0.28f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 4));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        
            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            ImVec2 Pos = ImGui::GetCursorScreenPos();
            ImVec2 Size = ImVec2(ContentBrowserTileView.GetTileSize(), ContentBrowserTileView.GetTileSize());
            
            DrawList->AddRectFilled(
                ImVec2(Pos.x + 3, Pos.y + 3),
                ImVec2(Pos.x + Size.x + 11, Pos.y + Size.y + 11),
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, 0.3f)),
                8.0f
            );
            
            ImGui::ImageButton("##", ImTexture, Size, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), TintColor);

            if (ImGui::IsItemHovered())
            {
                DrawList->AddRect(
                    Pos, 
                    ImVec2(Pos.x + Size.x + 8, Pos.y + Size.y + 8), 
                    ImGui::ColorConvertFloat4ToU32(ImVec4(0.4f, 0.6f, 0.9f, 0.7f)), 
                    8.0f, 
                    0, 
                    2.0f
                );
            }
            
            if (Item->IsSelected())
            {
                DrawList->AddRect(
                    Pos, 
                    ImVec2(Pos.x + Size.x + 8, Pos.y + Size.y + 8), 
                    ImGui::ColorConvertFloat4ToU32(ImVec4(0.6f, 0.6f, 0.1f, 0.9f)), 
                    8.0f, 
                    0, 
                    2.5f
                ); 
            }
        
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
            
            if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl))
                {
                    return FTileViewItem::EClickState::SingleWithCtrl;
                }
                
                return FTileViewItem::EClickState::Single;
            }
            
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                return FTileViewItem::EClickState::Double;
            }
        
            return FTileViewItem::EClickState::None;
        };
        
        ContentBrowserTileViewContext.ItemDoubleClickedFunction = [this] (FTileViewItem* Item)
        {
            FContentBrowserTileViewItem* ContentItem = static_cast<FContentBrowserTileViewItem*>(Item);
            FFixedString Path {ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().size()};
            
            if (ContentItem->IsDirectory())
            {
                SelectedPath = Move(Path);
                RefreshContentBrowser();
            }
            else if (ContentItem->IsAsset())
            {
                if (const FAssetData* Asset = FAssetRegistry::Get().GetAssetByPath(Path))
                {
                    ToolContext->OpenAssetEditor(Asset->AssetGUID);
                }
            }
            else if (ContentItem->GetIconKind() == EIconKind::Markup || ContentItem->GetIconKind() == EIconKind::Stylesheet)
            {
                // Files with an in-engine editor (.rml/.rcss) open as editor tabs.
                ToolContext->OpenFileEditor(ContentItem->GetVirtualPath());
            }
            else
            {
                // No in-engine editor (e.g. .cs): hand the native file to the OS so it opens in the
                // user's associated app (Rider / Visual Studio / VS Code for C#).
                Platform::LaunchURL(UTF8_TO_TCHAR(ContentItem->GetPathSource().data()));
            }
        };

        ContentBrowserTileViewContext.ItemSelectedFunction = [this] (FTileViewItem* Item)
        {
            
        };
        
        ContentBrowserTileViewContext.DrawItemContextMenuFunction = [this] (const TVector<FTileViewItem*>& Items)
        {
            bool bMultipleItems = Items.size() > 1;
            
            for (FTileViewItem* Item : Items)
            {
                FContentBrowserTileViewItem* ContentItem = static_cast<FContentBrowserTileViewItem*>(Item);

                if (bMultipleItems)
                {
                    continue;
                }

                DrawAssetContextMenu(ContentItem);

            }
        };

        ContentBrowserTileViewContext.RebuildTreeFunction = [this] (FTileViewWidget* Tree)
        {
            // The Filter menu toggles asset classes. Directories, scripts, and loose files are not
            // class-filterable, so they are always shown; assets are hidden when their class is off.
            auto PassesFilter = [this](const VFS::FFileInfo& Info) -> bool
            {
                if (!Info.IsLAsset())
                {
                    return true;
                }

                const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(FStringView(Info.VirtualPath.c_str(), Info.VirtualPath.size()));
                if (Data == nullptr)
                {
                    return true;
                }

                auto It = FilterState.find(Data->AssetClass);
                return It == FilterState.end() || It->second;
            };

            TVector<VFS::FFileInfo> SortedPaths;

            VFS::DirectoryIterator(SelectedPath, [&](const VFS::FFileInfo& FileInfo)
            {
                if (FileInfo.IsDirectory())
                {
                    // Hide dot-entries (e.g. the .lmeta sidecar tree) and build/IDE folders.
                    if (ShouldHideDirectory(FileInfo))
                    {
                        return;
                    }
                }
                else
                {
                    // Only surface extensions the engine actually authors/consumes; everything else
                    // (csproj, sidecars, IDE cruft) stays hidden.
                    if (!IsBrowsableFileExtension(FileInfo.GetExt()) || !PassesFilter(FileInfo))
                    {
                        return;
                    }
                }

                SortedPaths.emplace_back(FileInfo);
            });
            
            eastl::sort(SortedPaths.begin(), SortedPaths.end(), [&](const VFS::FFileInfo& LHS, const VFS::FFileInfo& RHS)
            {
                if (LHS.IsDirectory() != RHS.IsDirectory())
                {
                    return LHS.IsDirectory();
                }
                
                return LHS.Name < RHS.Name;
            });
            
            for (const VFS::FFileInfo& Info : SortedPaths)
            {
                const bool bProtected = IsProtectedRoot(FStringView(Info.VirtualPath.c_str(), Info.VirtualPath.size()));
                ContentBrowserTileView.AddItemToTree<FContentBrowserTileViewItem>(nullptr, Info, bProtected);
            }
        };

        ContentBrowserTileViewContext.KeyPressedFunction = [this] (FTileViewItem& Item, ImGuiKey Key) -> bool
        {
            if (Key == ImGuiKey_F2)
            {
                FContentBrowserTileViewItem* ContentItem = static_cast<FContentBrowserTileViewItem*>(&Item);
                PushRenameModal(ContentItem);
                return true;
            }

            if (Key == ImGuiKey_Delete)
            {
                FContentBrowserTileViewItem* ContentItem = static_cast<FContentBrowserTileViewItem*>(&Item);
                if (ContentItem->IsProtected())
                {
                    ImGuiX::Notifications::NotifyError("Cannot delete a core directory");
                    return true;
                }
                
                OpenDeletionWarningPopup(ContentItem);
                return true;
            }

            return false;
        };
        
        DirectoryContext.ItemContextMenuFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {

        };

        DirectoryContext.DragDropFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FContentBrowserListViewItemData& Data = Tree.Get<FContentBrowserListViewItemData>(Item);

            const DragDrop::FPayload* Peek = DragDrop::PeekPayload();
            if (Peek == nullptr || !DragDrop::IsDelivered())
            {
                return;
            }

            FStringView SourcePath;
            if (Peek->Kind == DragDrop::EPayloadKind::Asset)
            {
                SourcePath = FStringView(Peek->AssetPath.c_str(), Peek->AssetPath.size());
            }
            else if (Peek->Kind == DragDrop::EPayloadKind::File)
            {
                SourcePath = FStringView(Peek->FilePath.c_str(), Peek->FilePath.size());
            }
            else
            {
                return;
            }

            HandleContentBrowserDragDrop(Data.Path, SourcePath);

            for (FTileViewItem* TileItem : ContentBrowserTileView.GetSelections())
            {
                auto* SourceItem = static_cast<FContentBrowserTileViewItem*>(TileItem);

                if (SourceItem->GetVirtualPath() == SourcePath)
                {
                    continue;
                }

                HandleContentBrowserDragDrop(Data.Path, SourceItem->GetVirtualPath());
            }
        };
        
        // Helper: add a single folder node, flag it as having lazy children if it actually has subdirectories.
        auto AddFolderNode = [this](FTreeListView& Tree, FTreeNodeID Parent, const VFS::FFileInfo& Info)
        {
            FFixedString DisplayName;
            DisplayName.append(LE_ICON_FOLDER).append(" ").append(Info.Name.begin(), Info.Name.end());

            FTreeNodeID ItemEntity = Tree.CreateNode(Parent, FStringView(DisplayName.data(), DisplayName.length()), Hash::GetHash64(Info.PathSource));
            Tree.EmplaceUserData<FContentBrowserListViewItemData>(ItemEntity).Path.assign(Info.VirtualPath.begin(), Info.VirtualPath.end());

            FTreeNodeDisplay& FolderDisplay = Tree.Get<FTreeNodeDisplay>(ItemEntity);
            FolderDisplay.IconText = LE_ICON_FOLDER;
            FolderDisplay.IconColor = ImVec4(0.93f, 0.79f, 0.36f, 1.0f);

            if (Info.VirtualPath == SelectedPath)
            {
                FTreeNodeState& State = Tree.Get<FTreeNodeState>(ItemEntity);
                State.bSelected = true;
            }

            // Probe for at least one visible subdirectory; if any exists, mark lazy so the arrow appears.
            bool bHasSubdirs = false;
            VFS::DirectoryIterator(Info.VirtualPath, [&](const VFS::FFileInfo& Child)
            {
                if (Child.IsDirectory() && !ShouldHideDirectory(Child))
                {
                    bHasSubdirs = true;
                }
            });
            if (bHasSubdirs)
            {
                Tree.MarkHasLazyChildren(ItemEntity);
            }
            return ItemEntity;
        };

        DirectoryContext.RebuildTreeFunction = [this, AddFolderNode](FTreeListView& Tree)
        {
            // Roots are always built; their immediate children are loaded on first expand.
            auto AddRoot = [&](const char* Path, const char* Label)
            {
                FFixedString Name;
                Name.assign(LE_ICON_FOLDER).append(" ").append(Label);
                FTreeNodeID RootItem = Tree.CreateNode(InvalidTreeNode, FStringView(Name.data(), Name.length()), Hash::GetHash64(FStringView(Path).data(), FStringView(Path).length()));
                Tree.EmplaceUserData<FContentBrowserListViewItemData>(RootItem).Path = Path;

                FTreeNodeDisplay& RootDisplay = Tree.Get<FTreeNodeDisplay>(RootItem);
                RootDisplay.IconText = LE_ICON_FOLDER;
                RootDisplay.IconColor = ImVec4(0.93f, 0.79f, 0.36f, 1.0f);

                Tree.MarkHasLazyChildren(RootItem);
                return RootItem;
            };
            // Primary mount roots: each project is a top-level node (Game, Engine, plugins). Expanding one
            // reveals its real on-disk subdirs -- for the game, Content (assets) and Scripts (C#).
            AddRoot("/Game", "Game");
            AddRoot("/Engine/Resources", "Engine");
            for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
            {
                if (Plugin->IsEnabled() && Plugin->IsContentMounted())
                {
                    const FString Alias = Plugin->GetMountAlias();
                    const char* Label = (!Alias.empty() && Alias[0] == '/') ? Alias.c_str() + 1 : Alias.c_str();
                    AddRoot(Alias.c_str(), Label);
                }
            }
        };

        DirectoryContext.BuildChildrenFunction = [this, AddFolderNode](FTreeListView& Tree, FTreeNodeID Parent)
        {
            FContentBrowserListViewItemData& Data = Tree.Get<FContentBrowserListViewItemData>(Parent);
            VFS::DirectoryIterator(FStringView(Data.Path.data(), Data.Path.length()), [&](const VFS::FFileInfo& Info)
            {
                if (!Info.IsDirectory() || ShouldHideDirectory(Info))
                {
                    return;
                }
                AddFolderNode(Tree, Parent, Info);
            });
        };

        DirectoryContext.ItemSelectedFunction = [this] (FTreeListView& Tree, FTreeNodeID Item, bool)
        {
            if (!Item.IsValid())
            {
                return;
            }

            FContentBrowserListViewItemData& Data = Tree.Get<FContentBrowserListViewItemData>(Item);

            SelectedPath = Data.Path;

            RefreshContentBrowser();
        };

        DirectoryContext.KeyPressedFunction = [this] (FTreeListView& Tree, FTreeNodeID Item, ImGuiKey Key) -> bool
        {
            return false;
        };
        
        DirectoryListView.MarkTreeDirty();
        ContentBrowserTileView.MarkTreeDirty();
    }

    void FContentBrowserEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        
    }
    
    // True while a Play-In-Editor or Simulate session is running. Deleting an asset out from under a
    // live world can free objects the simulation still references, so deletes are blocked meanwhile.
    static bool IsAnyWorldPlayingOrSimulating()
    {
        if (GWorldManager == nullptr)
        {
            return false;
        }
        for (const TUniquePtr<FWorldContext>& Context : GWorldManager->GetContexts())
        {
            if (Context && (Context->Type == EWorldType::Game || Context->Type == EWorldType::Simulation))
            {
                return true;
            }
        }
        return false;
    }

    void FContentBrowserEditorTool::EndFrame()
    {
        bool bWroteSomething = false;

        // Drop (don't queue) any delete requests while playing/simulating -- one notification covers them.
        const bool bWorldActive = IsAnyWorldPlayingOrSimulating();
        bool bBlockNotified = false;

        ActionRegistry.ProcessAllOf<FPendingDestroy>([&] (const FPendingDestroy& Destroy)
        {
            if (bWorldActive)
            {
                if (!bBlockNotified)
                {
                    ImGuiX::Notifications::NotifyError("Cannot delete assets while playing or simulating. Stop play first.");
                    bBlockNotified = true;
                }
                return;
            }

            if (VFS::IsDirectory(Destroy.PendingDestroy))
            {
                // Text-asset sidecars live in the hidden .lmeta tree (not under this folder), so collect
                // contained text files first and drop their identities explicitly after the bulk remove.
                TVector<FFixedString> TextPaths;
                VFS::RecursiveDirectoryIterator(Destroy.PendingDestroy, [&](const VFS::FFileInfo& FileInfo)
                {
                    if (FileInfo.IsDirectory()) return;
                    const FStringView Vp(FileInfo.VirtualPath.c_str(), FileInfo.VirtualPath.size());
                    if (TextAsset::IsTextAssetPath(Vp))
                    {
                        TextPaths.emplace_back(FileInfo.VirtualPath);
                    }
                });

                VFS::RemoveAll(Destroy.PendingDestroy);

                for (const FFixedString& Tp : TextPaths)
                {
                    FAssetRegistry::Get().TextAssetDeleted(FStringView(Tp.c_str(), Tp.size()));
                }

                ImGuiX::Notifications::NotifySuccess("Deleted Directory {0}", Destroy.PendingDestroy);
                bWroteSomething = true;
                return;
            }

            if (VFS::HasExtension(Destroy.PendingDestroy, ".lasset"))
            {
                CObject* AliveObject = nullptr;
                if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Destroy.PendingDestroy))
                {
                    if (CObject* Object = FindObject<CObject>(Data->AssetGUID))
                    {
                        AliveObject = Object;
                        if (AliveObject->IsA<CWorld>())
                        {
                            ImGuiX::Notifications::NotifyError("Cannot destroy a world that's open {0}", Destroy.PendingDestroy);
                            return;
                        }
                    }
                }

                // Deleting a prefab asset deletes its placed instances in every open world (-style).
                // Pin it first: dropping the instances' strong SourcePrefab refs could otherwise free the
                // prefab out from under OnDestroyAsset/DestroyPackage below. Detached subtrees are untracked
                // and survive automatically.
                TObjectPtr<CObject> KeepAlive = AliveObject;
                if (AliveObject != nullptr && AliveObject->IsA<CPrefab>())
                {
                    static_cast<CPrefab*>(AliveObject)->DestroyAllInstancesInLoadedWorlds();
                }

                if (AliveObject)
                {
                    ToolContext->OnDestroyAsset(AliveObject);
                }

                if (CPackage::DestroyPackage(Destroy.PendingDestroy))
                {
                    ImGuiX::Notifications::NotifySuccess("Deleted Asset {0}", Destroy.PendingDestroy);
                    bWroteSomething = true;
                }
                return;
            }

            // Plain file (script, widget, audio, etc.), disk-level remove only.
            if (VFS::Remove(Destroy.PendingDestroy))
            {
                // Drop the text-asset identity + its sidecar.
                if (TextAsset::IsTextAssetPath(Destroy.PendingDestroy))
                {
                    FAssetRegistry::Get().TextAssetDeleted(Destroy.PendingDestroy);
                }
                ImGuiX::Notifications::NotifySuccess("Deleted {0}", Destroy.PendingDestroy);
                bWroteSomething = true;
            }
            else
            {
                ImGuiX::Notifications::NotifyError("Failed to delete {0}", Destroy.PendingDestroy);
            }
		});
        
        ActionRegistry.ProcessAllOf<FPendingRename>([&](FPendingRename& Rename)
        {
            FStringView Extension = VFS::Extension(Rename.OldName);

            if (Extension == ".lasset")
            {
                // RenamePackage owns the disk move + in-memory rename; only update registry on success.
                if (!CPackage::RenamePackage(Rename.OldName, Rename.NewName))
                {
                    ImGuiX::Notifications::NotifyError("Rename Failed: {0}", Rename.OldName);
                    return;
                }

                FAssetRegistry::Get().AssetRenamed(Rename.OldName, Rename.NewName);
                ImGuiX::Notifications::NotifySuccess("Rename Success");
                bWroteSomething = true;
            }
            else if (Extension.empty())
            {
                // Snapshot contained .lasset files before touching the filesystem to map old→new paths.
                struct FFolderRenameEntry
                {
                    FFixedString OldPath;
                    FFixedString NewPath;
                };
                TVector<FFolderRenameEntry> Entries;

                FStringView OldFolder(Rename.OldName.data(), Rename.OldName.size());
                FStringView NewFolder(Rename.NewName.data(), Rename.NewName.size());

                VFS::RecursiveDirectoryIterator(Rename.OldName, [&](const VFS::FFileInfo& FileInfo)
                {
                    if (FileInfo.IsDirectory() || !FileInfo.IsLAsset())
                    {
                        return;
                    }

                    FStringView Old(FileInfo.VirtualPath.data(), FileInfo.VirtualPath.size());
                    if (!Old.starts_with(OldFolder))
                    {
                        return;
                    }

                    FFixedString NewPath(NewFolder.data(), NewFolder.size());
                    NewPath.append(Old.data() + OldFolder.size(), Old.size() - OldFolder.size());

                    Entries.push_back({ FFixedString(Old.data(), Old.size()), Move(NewPath) });
                });

                if (!VFS::Rename(Rename.OldName, Rename.NewName))
                {
                    ImGuiX::Notifications::NotifyError("Folder Rename Failed: {0}", Rename.OldName);
                    return;
                }

                // File names unchanged (only directory portion); no content rewrite needed.
                for (const FFolderRenameEntry& Entry : Entries)
                {
                    CPackage::OnPackageMovedExternally(Entry.OldPath, Entry.NewPath);
                    FAssetRegistry::Get().AssetRenamed(Entry.OldPath, Entry.NewPath);
                }

                // Relocate the identities (and sidecars) of every contained text asset.
                FAssetRegistry::Get().TextAssetFolderRenamed(OldFolder, NewFolder);

                ImGuiX::Notifications::NotifySuccess("Folder Rename Success");
                bWroteSomething = true;
            }
            else
            {
                // Plain file (non-asset)
                if (!VFS::Rename(Rename.OldName, Rename.NewName))
                {
                    ImGuiX::Notifications::NotifyError("Rename Failed: {0}", Rename.OldName);
                    return;
                }
                // Carry the text-asset identity (sidecar) across the rename so references survive.
                if (TextAsset::IsTextAssetPath(Rename.OldName) || TextAsset::IsTextAssetPath(Rename.NewName))
                {
                    FAssetRegistry::Get().TextAssetRenamed(Rename.OldName, Rename.NewName);
                }
                ImGuiX::Notifications::NotifySuccess("Rename Success");
                bWroteSomething = true;
            }
        });


        if (bWroteSomething)
        {
            RefreshContentBrowser();
        }
    }
    
    void FContentBrowserEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGuiID topDockID = 0, bottomLeftDockID = 0, bottomCenterDockID = 0, bottomRightDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Down, 0.5f, &bottomCenterDockID, &topDockID);
        ImGui::DockBuilderSplitNode(bottomCenterDockID, ImGuiDir_Right, 0.66f, &bottomCenterDockID, &bottomLeftDockID);
        ImGui::DockBuilderSplitNode(bottomCenterDockID, ImGuiDir_Right, 0.5f, &bottomRightDockID, &bottomCenterDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName("Content").c_str(), bottomCenterDockID);
    }

    void FContentBrowserEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Browse",
            "Left panel is the directory tree, right is the tile grid for the selected folder. "
            "Double-click a folder to enter, double-click an asset to open its editor.");
        DrawHelpTextRow("Create",
            "Right-click empty space in the tile grid for the New menu (Material, Prefab, C# script, etc). "
            "Right-click a folder for create-in-place.");
        DrawHelpTextRow("Import",
            "Drag external files (FBX, PNG, WAV, ...) onto the tile grid to import. "
            "Each importer maps to a CObject asset class.");
        DrawHelpTextRow("Drag & Drop",
            "Drag an asset tile into the world viewport, outliner, or a property field that accepts its type. "
            "Filtering happens at drop-time based on the asset class.");
        DrawHelpTextRow("Filter",
            "Filter menu hides asset classes you don't want to see. View Options changes tile size.");
        DrawHelpTextRow("Rename / Delete",
            "F2 renames; Delete removes. Renames update inbound references via redirectors.");
    }

    void FContentBrowserEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        if (ImGui::BeginMenu(LE_ICON_FILTER " Filter"))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 2));

            for (auto& [Name, State] : FilterState)
            {
                if (ImGui::Checkbox(Name.c_str(), &State))
                {
                    RefreshContentBrowser();
                }
            }

            ImGui::PopStyleVar(2);
            ImGui::EndMenu();
        }

		if (ImGui::BeginMenu(LE_ICON_COGS " View Options"))
        {
            ImGui::SetNextItemWidth(128.0f);
            if (ImGui::SliderFloat("##Zoom", &ContentBrowserTileSize, 46.0f, 256.0f, "Tile: %.1fx"))
            {
                GetMutableDefault<CContentBrowserSettings>()->TileSize = ContentBrowserTileSize;
                GConfig->SaveSettings(CContentBrowserSettings::StaticClass());
                ContentBrowserTileView.SetTileSize(ContentBrowserTileSize);
            }
            
            ImGui::EndMenu();
        }
    }

    void FContentBrowserEditorTool::HandleContentBrowserDragDrop(FStringView DropPath, FStringView PayloadPath)
    {
        size_t Pos = PayloadPath.find_last_of('/');
        FStringView DirName = (Pos != FString::npos) ? PayloadPath.substr(Pos + 1) : PayloadPath;
        
        FFixedString OldName(PayloadPath.data(), PayloadPath.length());
        FFixedString NewName = Paths::Combine(DropPath, DirName);

        ActionRegistry.EnqueueAction<FPendingRename>(FPendingRename{ OldName, NewName });
    }

    void FContentBrowserEditorTool::OpenDeletionWarningPopup(const FContentBrowserTileViewItem* Item, const TFunction<void(EYesNo)>& Callback)
    {
        if (VFS::IsEmpty(Item->GetVirtualPath()))
        {
            if (Callback)
            {
                Callback(EYesNo::Yes);
            }
            ActionRegistry.EnqueueAction<FPendingDestroy>(FPendingDestroy{ FFixedString(Item->GetVirtualPath().data(), Item->GetVirtualPath().size()) });
        }
        else if (Dialogs::Confirmation("Confirm Deletion", "Are you sure you want to delete \"{0}\"?\n""\nThis action cannot be undone.", Item->GetName()))
        {
            if (Callback)
            {
                Callback(EYesNo::Yes);
            }
            ActionRegistry.EnqueueAction<FPendingDestroy>(FPendingDestroy{ FFixedString(Item->GetVirtualPath().data(), Item->GetVirtualPath().size()) });
        }
        
        if (Callback)
        {
            Callback(EYesNo::No);
        }
    }

    void FContentBrowserEditorTool::OnProjectLoaded()
    {
        // Tear down any prior set; clearing is safe since FDirectoryWatcher's
        // destructor stops its worker thread. Project reload rebuilds from scratch.
        for (FContentWatcher& W : Watchers)
        {
            if (W.Watcher)
            {
                W.Watcher->Stop();
            }
        }
        Watchers.clear();

        auto TrimTrailingSeparators = [](FFixedString& Path)
        {
            while (!Path.empty() && (Path.back() == '/' || Path.back() == '\\'))
            {
                Path.pop_back();
            }
        };

        auto SpawnWatcher = [this, &TrimTrailingSeparators](FFixedString DiskRoot, FStringView VirtualPrefix)
        {
            Paths::Normalize(DiskRoot);
            TrimTrailingSeparators(DiskRoot);
            if (DiskRoot.empty())
            {
                return;
            }

            FContentWatcher Entry;
            Entry.VirtualPrefix.assign_convert(VirtualPrefix.data(), VirtualPrefix.size());
            Entry.WatchRootLen = DiskRoot.size();
            Entry.Watcher      = MakeUnique<FDirectoryWatcher>();

            // Capture prefix + root length by value so the callback is self-contained
            // even if Watchers reallocates (the TUniquePtr'd watcher itself is stable).
            const FFixedString Prefix = Entry.VirtualPrefix;
            const size_t       RootLen = Entry.WatchRootLen;

            auto MakeVirtualPath = [Prefix, RootLen](FStringView AbsPath) -> FFixedString
            {
                FFixedString Out;
                Out.append_convert(Prefix.c_str(), Prefix.size());
                if (AbsPath.size() > RootLen)
                {
                    FStringView Tail = AbsPath.substr(RootLen);
                    if (!Tail.empty() && Tail.front() != '/')
                    {
                        Out.append_convert("/");
                    }
                    Out.append_convert(Tail.data(), Tail.size());
                }
                return Out;
            };

            Entry.Watcher->Watch(DiskRoot, [this, MakeVirtualPath](const FFileEvent& Event)
            {
                const FFixedString RelativePath = MakeVirtualPath(Event.Path);
                const FStringView  RelView(RelativePath.c_str(), RelativePath.size());

                // Our own hidden identity sidecars: ignore so writing one doesn't churn the browser.
                if (TextAssetSidecar::IsSidecarPath(RelView))
                {
                    return;
                }
                
                FCoreDelegates::OnContentFileModified.Broadcast(RelView);
                
                if (TextAsset::IsTextAssetPath(RelView))
                {
                    const EFileAction Action  = Event.Action;
                    const FFixedString NewPath = RelativePath;
                    const FFixedString OldPath = (Action == EFileAction::Renamed) ? MakeVirtualPath(Event.OldPath) : FFixedString();

                    MainThread::Enqueue([Action, NewPath, OldPath]
                    {
                        FAssetRegistry& Reg = FAssetRegistry::Get();
                        const FStringView New(NewPath.c_str(), NewPath.size());
                        switch (Action)
                        {
                        case EFileAction::Added:   Reg.TextAssetCreated(New); break;
                        case EFileAction::Removed: Reg.TextAssetDeleted(New); break;
                        case EFileAction::Renamed: Reg.TextAssetRenamed(FStringView(OldPath.c_str(), OldPath.size()), New); break;
                        default: break;
                        }
                    });
                }

                // Text edits and C# sources want a browser refresh for add/remove/rename
                // (C# isn't a text asset, so check its extension explicitly).
                const bool bIsCSharp = VFS::HasExtension(Event.Path, ".cs");
                if ((TextAsset::IsTextAssetPath(RelView) || bIsCSharp) && Event.Action != EFileAction::Modified)
                {
                    RefreshContentBrowser();
                }

                // A C# source added/removed/renamed (in the browser or an external editor) changes what
                // compiles -> recompile + regenerate the IDE project automatically, so the user never has to
                // hit "Reload Scripts". Coalesced, and marshalled to the game thread (CLR ops aren't thread-
                // safe). ReloadScripts also self-heals the .csproj. (Content edits = Modified are left to the
                // explicit reload; create/delete/rename are the browser operations.)
                if (bIsCSharp && Event.Action != EFileAction::Modified)
                {
                    if (!GScriptReloadQueued.exchange(true))
                    {
                        MainThread::Enqueue([]
                        {
                            GScriptReloadQueued.store(false);
                            DotNet::ReloadScripts();
                        });
                    }
                }
            });

            Watchers.emplace_back(Move(Entry));
        };

        // Project's Content (assets), under the /Game mount. Always present.
        SpawnWatcher(FFixedString(GEditorEngine->GetProjectContentDirectory()), FStringView("/Game/Content"));

        // Project's Scripts (C# sources), the sibling of Content under /Game. The callback refreshes the
        // browser for .cs add/remove/rename.
        SpawnWatcher(FFixedString(GEditorEngine->GetProjectScriptsDirectory()), FStringView("/Game/Scripts"));

        // Every enabled plugin with a content mount. Same callback shape,
        // virtual prefix is the plugin's mount alias ("/<PluginName>").
        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (!Plugin->IsEnabled())
            {
                continue;
            }
            if (!Plugin->IsContentMounted())
            {
                continue;
            }
            const FString Disk  = Plugin->GetContentDirectory();
            const FString Mount = Plugin->GetMountAlias();
            SpawnWatcher(FFixedString(Disk.c_str(), Disk.size()),
                         FStringView(Mount.c_str(), Mount.size()));
        }

        // Land on the project's /Game root so the browser shows content immediately after a
        // load instead of sitting on a stale/empty path.
        SelectedPath = "/Game";
        RefreshContentBrowser();
    }

    void FContentBrowserEditorTool::TryImport(const FFixedString& Path)
    {
        const TVector<CFactory*>& Factories = CFactoryRegistry::Get().GetFactories();
        for (CFactory* Factory : Factories)
        {
            if (!Factory->CanImport())
            {
                continue;
            }
        
            FStringView Ext = VFS::Extension(Path);
            if (!Factory->IsExtensionSupported(Ext))
            {
                continue;
            }
            
            
            FStringView FileName = VFS::FileName(Path);
            FFixedString DestinationPath = Paths::Combine(SelectedPath, FileName);
            DestinationPath = VFS::MakeUniqueFilePath(DestinationPath);
            
            if (Factory->HasImportDialogue())
            {
                struct FModalState
                {
                    TUniquePtr<Import::FImportSettings> ImportSettings;
                    bool bShouldClose = false;
                };

                // Prepare the import off-thread first (parsing shows the slow-task popup);
                // the options dialog is pushed only once the settings have landed.
                Factory->PrepareImportAsync(Path, DestinationPath,
                    [this, Factory, Path, DestinationPath](TUniquePtr<Import::FImportSettings> Settings)
                    {
                        if (!Settings)
                        {
                            ImGuiX::Notifications::NotifyError("Failed to import: \"{0}\"", Path);
                            return;
                        }

                        auto SharedState = MakeShared<FModalState>();
                        SharedState->ImportSettings = Move(Settings);

                        ToolContext->PushModal("Import", {700, 800},
                            [this, Factory, Path, DestinationPath, SharedState]() mutable
                            {
                                if (Factory->DrawImportDialogue(Path, DestinationPath, SharedState->ImportSettings, SharedState->bShouldClose))
                                {
                                    Task::AsyncTask(1, 1, [this, Factory, Path, DestinationPath, ImportSettings = Move(SharedState->ImportSettings)](uint32, uint32, uint32)
                                    {
                                        Factory->Import(Path, DestinationPath, ImportSettings.get());

                                        MainThread::Enqueue([this, Path]()
                                        {
                                            RefreshContentBrowser();
                                            ImGuiX::Notifications::NotifySuccess("Successfully Imported: \"{0}\"", Path);
                                        });
                                    });
                                }

                                return SharedState->bShouldClose;
                            });
                    });
            }
            else
            {
                Task::AsyncTask(1, 1, [this, Factory, Path = Move(Path), PathString = Move(DestinationPath)] (uint32, uint32, uint32)
                {
                    Factory->Import(Path, PathString, nullptr);

                    MainThread::Enqueue([this, Path = Move(Path)] ()
                    {
                        RefreshContentBrowser();
                        ImGuiX::Notifications::NotifySuccess("Successfully Imported: \"{0}\"", Path);
                    });
                });
            }
        }
    }

    void FContentBrowserEditorTool::PushRenameModal(FContentBrowserTileViewItem* ContentItem)
    {
        ToolContext->PushModal("Rename", ImVec2(480.0f, 300.0f), [this, ContentItem, RenameState = MakeUnique<FRenameModalState<>>()]
        {
            if (!RenameState->IsValid())
            {
                RenameState->Initialize(ContentItem->GetName());
            }
            
            const ImGuiStyle& style = ImGui::GetStyle();
            const float ContentWidth = ImGui::GetContentRegionAvail().x;
            
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::MediumBold);
            ImGuiX::TextColored(ImVec4(0.9f, 0.9f, 0.95f, 1.0f), LE_ICON_ARCHIVE_EDIT " Rename {0}", ContentItem->IsDirectory() ? "Folder" : "Asset");
            ImGuiX::Font::PopFont();
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Spacing();
            
            ImGuiX::TextColored(ImVec4(0.6f, 0.6f, 0.65f, 1.0f), "Current name:");
            
            ImGui::SameLine();
            
            ImGuiX::TextColored(ImVec4(0.85f, 0.85f, 0.9f, 1.0f), "{0}", ContentItem->GetName());
            
            ImGui::Spacing();
            ImGui::Spacing();
            
            ImGuiX::TextColoredUnformatted(ImVec4(0.6f, 0.6f, 0.65f, 1.0f), "New name:");
            
            ImGui::Spacing();
            
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.2f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.25f, 0.25f, 0.3f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            
            ImGui::SetNextItemWidth(-1);
            
            bool bSubmitted = ImGui::InputText("##RenameInput", RenameState->CStr(), RenameState->Capacity(), ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_EnterReturnsTrue);
            
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
            
            ImGui::Spacing();
            ImGui::Spacing();
            
            bool bIsValid = RenameState->IsValid();
            bool bNameUnchanged = strcmp(RenameState->CStr(), ContentItem->GetName().data()) == 0;
            FString ValidationMessage;
            bool bHasError = false;
            
            if (RenameState->IsValid())
            {
                if (bNameUnchanged)
                {
                    ValidationMessage = "Name unchanged - please enter a different name";
                    bHasError = true;
                    bIsValid = false;
                }
                else
                {
                    FStringView Extension = ContentItem->GetExtension();
                    FStringView PathNoExt = VFS::RemoveExtension(ContentItem->GetVirtualPath());
                    FFixedString TestPath = Paths::Combine(PathNoExt, RenameState->CStr());
                    TestPath.append_convert(Extension.data(), Extension.length());
                    
                    if (VFS::Exists(TestPath))
                    {
                        ValidationMessage = std::format("Path already exists: {}", TestPath.c_str()).c_str();
                        bHasError = true;
                        bIsValid = false;
                    }
                }
            }
            
            if (bHasError && !ValidationMessage.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.3f, 0.1f, 0.1f, 0.3f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.8f, 0.2f, 0.2f, 0.4f));
                
                ImGui::BeginChild("##ValidationError", ImVec2(-1, 45.0f), true);
                
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::Text(LE_ICON_ALERT_OCTAGON);
                ImGui::SameLine();
                ImGui::TextWrapped("%s", ValidationMessage.c_str());
                ImGui::PopStyleColor();
                
                ImGui::EndChild();
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);
                
                ImGui::Spacing();
            }
            
            if (bSubmitted && bIsValid)
            {
                FStringView PathNoExt = VFS::RemoveExtension(ContentItem->GetVirtualPath());
                FFixedString TestPath = Paths::Combine(VFS::Parent(PathNoExt), RenameState->CStr());
                TestPath.append_convert(ContentItem->GetExtension());
                
                ActionRegistry.EnqueueAction<FPendingRename>(FPendingRename{ FFixedString(ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().length()), TestPath });
                return true;
            }
            
            ImGui::Spacing();
            
            ImGui::Separator();
            ImGui::Spacing();

            constexpr float ButtonHeight = 32.0f;
            const float ButtonWidth = (ContentWidth - style.ItemSpacing.x) * 0.5f;
            
            if (!bIsValid)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.9f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.45f, 0.85f, 1.0f));
            }
            
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            
            if (ImGui::Button(LE_ICON_CHECK " Rename", ImVec2(ButtonWidth, ButtonHeight)))
            {
                if (bIsValid)
                {
                    ImGui::PopStyleColor(3);
                    ImGui::PopStyleVar();

                    FStringView PathNoExt = VFS::RemoveExtension(ContentItem->GetVirtualPath());
                    FFixedString TestPath = Paths::Combine(VFS::Parent(PathNoExt), RenameState->CStr());
                    TestPath.append_convert(ContentItem->GetExtension());
                
                    ActionRegistry.EnqueueAction<FPendingRename>(FPendingRename{ FFixedString(ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().length()), TestPath });
                    return true;
                }
            }
            
            ImGui::PopStyleVar();
            
            if (!bIsValid)
            {
                ImGui::PopItemFlag();
                ImGui::PopStyleVar();
            }
            else
            {
                ImGui::PopStyleColor(3);
            }
            
            ImGui::SameLine();
            
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.25f, 0.27f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.17f, 1.0f));
            
            if (ImGui::Button(LE_ICON_CANCEL " Cancel", ImVec2(ButtonWidth, ButtonHeight)))
            {
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar();
                
                return true;
            }
            
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                return true;
            }
            
            return false;
        });
    }

    void FContentBrowserEditorTool::DrawDirectoryBrowser(bool bIsFocused, ImVec2 Size)
    {
        ImGui::BeginChild("Directories", Size, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);

        DirectoryListView.Draw(DirectoryContext);
        
        ImGui::EndChild();
    }

    void FContentBrowserEditorTool::DrawContentBrowser(bool bIsFocused, ImVec2 Size)
    {
        constexpr float Padding = 1.0f;

        ImVec2 AdjustedSize = ImVec2(Size.x - 2 * Padding, 0.0f);

        ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(Padding, Padding));

        ImGui::BeginChild("Content", AdjustedSize, true, ImGuiWindowFlags_None);
        
        if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        {
            ImGui::OpenPopup("ContentContextMenu");
            ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f, 0.0f), ImVec2(360.0f, FLT_MAX));
        }

        PushContextMenuWindowStyle();

        if (ImGui::BeginPopup("ContentContextMenu"))
        {
            PushContextMenuItemStyle();
            DrawContentDirectoryContextMenu();
            PopContextMenuItemStyle();

            ImGui::EndPopup();
        }

        PopContextMenuWindowStyle();
        
        if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            ContentBrowserTileView.ClearSelections();
        }
        
        if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsKeyPressed(ImGuiKey_Delete) && !ContentBrowserTileView.GetSelections().empty())
        {
            if (Dialogs::Confirmation("Confirm Deletion", "Are you sure you want to delete these files/directories?\n This action cannot be undone."))
            {
                for (FTileViewItem* Selection : ContentBrowserTileView.GetSelections())
                {
                    FContentBrowserTileViewItem* ContentBrowserItem = static_cast<FContentBrowserTileViewItem*>(Selection);
                    DEBUG_ASSERT(Selection->IsSelected());
                    
                    ActionRegistry.EnqueueAction<FPendingDestroy>(
                    FPendingDestroy
                    { 
                        FFixedString(ContentBrowserItem->GetVirtualPath().data(), ContentBrowserItem->GetVirtualPath().size())
                    });
                }
            }
        }
        
        ImGui::BeginHorizontal("Breadcrumbs");

        // Walk the virtual path segment-by-segment so every mount root (Content, Scripts, Engine,
        // plugins) renders the same way. The first segment is the mount alias, shown with its tree label.
        auto RootSegmentLabel = [](FStringView) -> const char*
        {
            return nullptr; // use the raw segment text (Game, Content, Scripts, Engine, plugins)
        };

        const FStringView FullPath(SelectedPath.c_str(), SelectedPath.size());
        size_t Cursor = 0;
        int CrumbIndex = 0;
        while (Cursor < FullPath.size())
        {
            while (Cursor < FullPath.size() && FullPath[Cursor] == '/') { ++Cursor; }
            if (Cursor >= FullPath.size()) { break; }

            const size_t SegStart = Cursor;
            while (Cursor < FullPath.size() && FullPath[Cursor] != '/') { ++Cursor; }
            const FStringView Segment   = FullPath.substr(SegStart, Cursor - SegStart);
            const FStringView CrumbPath = FullPath.substr(0, Cursor);

            if (CrumbIndex > 0)
            {
                ImGui::TextUnformatted(LE_ICON_ARROW_RIGHT);
            }

            ImGui::PushID(CrumbIndex);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 2));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));

            const char* Label = (CrumbIndex == 0) ? RootSegmentLabel(Segment) : nullptr;
            FFixedString Display;
            if (Label != nullptr) { Display.assign(Label); }
            else                  { Display.assign(Segment.data(), Segment.size()); }

            if (ImGui::Button(Display.c_str()))
            {
                SelectedPath.assign(CrumbPath.data(), CrumbPath.size());
                ContentBrowserTileView.MarkTreeDirty();
            }

            ImGui::PopStyleVar(2);
            ImGui::PopID();

            ++CrumbIndex;
        }

        ImGui::EndHorizontal();

        ImGui::Separator();

        // Per-item context menu popups are opened from inside the tile view's Draw().
        // Push the popup window styles here so they apply when those popups are created.
        PushContextMenuWindowStyle();
        ContentBrowserTileView.Draw(ContentBrowserTileViewContext);
        PopContextMenuWindowStyle();

        ImVec2 ChildMin = ImGui::GetWindowPos();
        ImVec2 ChildMax = ImVec2(ChildMin.x + ImGui::GetWindowWidth(), ChildMin.y + ImGui::GetWindowHeight());
        
        ImRect Rect(ChildMin, ChildMax);

        ActionRegistry.ProcessAllOf<FPendingOSDrop>([&](const FPendingOSDrop& Drop)
        {
            if (Rect.Contains(Drop.MousePos))
            {
                TryImport(Drop.Path);
            }
		});
        
        ImGui::EndChild();
    
    }

    void FContentBrowserEditorTool::DrawAssetContextMenu(FContentBrowserTileViewItem* ContentItem)
    {
        const bool bIsAsset      = ContentItem->IsAsset();
        const bool bIsDirectory  = ContentItem->IsDirectory();
        const bool bIsCSharp     = ContentItem->GetIconKind() == EIconKind::CSharpScript;
        const bool bIsProtected  = ContentItem->IsProtected();
        // UI markup (.rml document + .rcss stylesheet) has an in-engine editor; everything else opens externally.
        const bool bHasInEngineEditor = ContentItem->GetIconKind() == EIconKind::Markup
                                     || ContentItem->GetIconKind() == EIconKind::Stylesheet;
        const FString  Extension = ContentItem->GetExtension();

        const char* HeaderIcon;
        ImVec4      HeaderTint;
        const char* TypeLabel;
        if (bIsDirectory)
        {
            HeaderIcon = LE_ICON_FOLDER_OPEN;
            HeaderTint = kMenuAccentFolder;
            TypeLabel  = "Folder";
        }
        else if (bIsAsset)
        {
            HeaderIcon = LE_ICON_FILE_DOCUMENT;
            HeaderTint = kMenuAccent;
            TypeLabel  = "Asset";
        }
        else if (bIsCSharp)
        {
            HeaderIcon = LE_ICON_LANGUAGE_CSHARP;
            HeaderTint = kMenuAccentScript;
            TypeLabel  = "C# Script";
        }
        else
        {
            HeaderIcon = LE_ICON_FILE;
            HeaderTint = ImVec4(0.78f, 0.78f, 0.82f, 1.0f);
            TypeLabel  = "File";
        }

        FFixedString TitleBuf(ContentItem->GetName().data(), ContentItem->GetName().size());
        FFixedString SubtitleBuf(ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().size());
        (void)TypeLabel;

        PushContextMenuItemStyle();

        DrawMenuHeader(HeaderIcon, TitleBuf.c_str(), SubtitleBuf.c_str(), HeaderTint);

        DrawMenuSection("OPEN");

        if (bIsDirectory)
        {
            if (ImGui::MenuItem(LE_ICON_FOLDER_OPEN " Open Folder", "Dbl-Click"))
            {
                SelectedPath = FFixedString(ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().size());
                RefreshContentBrowser();
            }
        }
        else if (bIsAsset)
        {
            if (ImGui::MenuItem(LE_ICON_FOLDER_OPEN " Open Asset", "Dbl-Click"))
            {
                FFixedString Path(ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().size());
                if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Path))
                {
                    ToolContext->OpenAssetEditor(Data->AssetGUID);
                }
            }
        }
        else
        {
            if (ImGui::MenuItem(LE_ICON_FOLDER_OPEN " Open", "Dbl-Click"))
            {
                if (bHasInEngineEditor)
                {
                    ToolContext->OpenFileEditor(ContentItem->GetVirtualPath());
                }
                else
                {
                    // .cs and other loose files: open in the OS-associated app.
                    Platform::LaunchURL(UTF8_TO_TCHAR(ContentItem->GetPathSource().data()));
                }
            }
            if (ImGui::MenuItem(LE_ICON_OPEN_IN_NEW " Open Externally"))
            {
                Platform::LaunchURL(UTF8_TO_TCHAR(ContentItem->GetPathSource().data()));
            }
        }

        if (ImGui::MenuItem(LE_ICON_MICROSOFT_WINDOWS " Show in Explorer"))
        {
            FString Parent = Paths::Parent(ContentItem->GetPathSource());
            Platform::LaunchURL(StringUtils::ToWideString(Parent).c_str());
        }

        DrawMenuSection("EDIT");

        if (ImGui::MenuItem(LE_ICON_RENAME " Rename", "F2", false, !bIsProtected))
        {
            PushRenameModal(ContentItem);
        }

        DrawMenuSection("CLIPBOARD");

        if (ImGui::MenuItem(LE_ICON_CONTENT_COPY " Copy Path"))
        {
            ImGui::SetClipboardText(ContentItem->GetVirtualPath().data());
            ImGuiX::Notifications::NotifyInfo("Path copied to clipboard");
        }
        if (ImGui::MenuItem(LE_ICON_TAG " Copy Name"))
        {
            FFixedString Name(ContentItem->GetName().data(), ContentItem->GetName().size());
            ImGui::SetClipboardText(Name.c_str());
            ImGuiX::Notifications::NotifyInfo("Name copied to clipboard");
        }
        if (bIsAsset)
        {
            if (ImGui::MenuItem(LE_ICON_LINK " Copy Reference"))
            {
                FFixedString Path(ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().size());
                if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Path))
                {
                    FFixedString Reference;
                    Reference.append("Asset(").append(Data->AssetClass.c_str()).append("'").append(Path.c_str()).append("')");
                    ImGui::SetClipboardText(Reference.c_str());
                    ImGuiX::Notifications::NotifyInfo("Reference copied to clipboard");
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        ImGui::PushStyleColor(ImGuiCol_Text,          kMenuDanger);
        const bool bWorldActive = IsAnyWorldPlayingOrSimulating();
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kMenuDangerHover);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.85f, 0.22f, 0.24f, 0.85f));
        const bool bDeleteClicked = ImGui::MenuItem(LE_ICON_TRASH_CAN " Delete", "Del", false, !bIsProtected && !bWorldActive);
        ImGui::PopStyleColor(3);

        if (bIsProtected)
        {
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Tiny);
            ImGui::PushStyleColor(ImGuiCol_Text, kMenuTextDim);
            const float OldX = ImGui::GetCursorPosX();
            ImGui::SetCursorPosX(OldX + 4.0f);
            ImGui::TextUnformatted(LE_ICON_LOCK " Protected, cannot be deleted");
            ImGui::PopStyleColor();
            ImGuiX::Font::PopFont();
        }
        else if (bWorldActive)
        {
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Tiny);
            ImGui::PushStyleColor(ImGuiCol_Text, kMenuTextDim);
            const float OldX = ImGui::GetCursorPosX();
            ImGui::SetCursorPosX(OldX + 4.0f);
            ImGui::TextUnformatted(LE_ICON_LOCK " Stop play to delete");
            ImGui::PopStyleColor();
            ImGuiX::Font::PopFont();
        }

        if (bDeleteClicked)
        {
            OpenDeletionWarningPopup(ContentItem);
        }

        PopContextMenuItemStyle();
    }
    
    void FContentBrowserEditorTool::DrawContentDirectoryContextMenu()
    {
        FStringView FolderName = VFS::FileName(FStringView(SelectedPath.c_str(), SelectedPath.size()), true);
        FFixedString FolderTitle(FolderName.data(), FolderName.size());
        if (FolderTitle.empty())
        {
            FolderTitle.assign("Content");
        }

        DrawMenuHeader(LE_ICON_FOLDER_OPEN, FolderTitle.c_str(), SelectedPath.c_str(), kMenuAccentFolder);

        // Scripts and Content are kept strictly separate: a Scripts/ folder only offers C# scripts, and
        // every other folder only offers assets/UI/imports. This is what stops scripts from landing outside
        // Scripts/ and assets from landing inside it.
        const bool bScriptDir = IsScriptDirectory(FStringView(SelectedPath.c_str(), SelectedPath.size()));

        DrawMenuSection("CREATE");

        if (ImGui::MenuItem(LE_ICON_FOLDER_PLUS " New Folder"))
        {
            FFixedString FinalPath = VFS::MakeUniqueFilePath(SelectedPath + "/NewFolder");
            VFS::CreateDir(FinalPath);
            RefreshContentBrowser();
        }

        // Aggregated asset creation submenu (factory-driven), grouped into per-category submenus.
        const TVector<CFactory*>& Factories = CFactoryRegistry::Get().GetFactories();
        if (!bScriptDir && ImGui::BeginMenu(LE_ICON_PLUS_BOX " New Asset"))
        {
            auto CreateFromFactory = [this](CFactory* Factory)
            {
                FFixedString Path = Paths::Combine(SelectedPath, Factory->GetDefaultAssetCreationName());
                CPackage::AddPackageExt(Path);
                Path = VFS::MakeUniqueFilePath(Path);

                if (Factory->HasCreationDialogue())
                {
                    ToolContext->PushModal("Create New", {500, 500}, [this, Factory, Path = Move(Path)]
                    {
                        bool bShouldClose = CFactory::ShowCreationDialogue(Factory, Path);
                        if (bShouldClose)
                        {
                            ImGuiX::Notifications::NotifySuccess("Successfully Created: \"{0}\"", Path);
                        }
                        return bShouldClose;
                    });
                }
                else if (CObject* Object = Factory->TryCreateNew(Path))
                {
                    if (CPackage::SavePackage(Object->GetPackage(), Path))
                    {
                        FAssetRegistry::Get().AssetCreated(Object);
                        ImGuiX::Notifications::NotifySuccess("Successfully Created: \"{0}\"", Path);
                    }
                    else
                    {
                        ImGuiX::Notifications::NotifyError("Failed to save new asset: \"{0}\"", Path);
                    }
                }
                else
                {
                    ImGuiX::Notifications::NotifyError("Failed to create new: \"{0}\"", Path);
                }
            };

            // Collect the creatable factories and their distinct categories.
            TVector<CFactory*> Creatable;
            TVector<FString>   Categories;
            for (CFactory* Factory : Factories)
            {
                if (Factory->CanImport() || Factory->GetAssetClass() == nullptr)
                {
                    continue;
                }
                Creatable.push_back(Factory);

                const FString Category = Factory->GetCategory();
                bool bSeen = false;
                for (const FString& Existing : Categories)
                {
                    if (Existing == Category) { bSeen = true; break; }
                }
                if (!bSeen)
                {
                    Categories.push_back(Category);
                }
            }

            eastl::sort(Categories.begin(), Categories.end());

            for (const FString& Category : Categories)
            {
                if (!ImGui::BeginMenu(Category.c_str()))
                {
                    continue;
                }

                TVector<CFactory*> InCategory;
                for (CFactory* Factory : Creatable)
                {
                    if (Factory->GetCategory() == Category)
                    {
                        InCategory.push_back(Factory);
                    }
                }
                eastl::sort(InCategory.begin(), InCategory.end(), [](CFactory* A, CFactory* B)
                {
                    return A->GetAssetName() < B->GetAssetName();
                });

                for (CFactory* Factory : InCategory)
                {
                    FString DisplayName = FString(LE_ICON_FILE_DOCUMENT_PLUS) + " " + Factory->GetAssetName();
                    if (ImGui::MenuItem(DisplayName.c_str()))
                    {
                        CreateFromFactory(Factory);
                    }
                }

                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        
        if (bScriptDir && ImGui::MenuItem(LE_ICON_LANGUAGE_CSHARP " New C# Script"))
        {
            FFixedString NewScriptPath = SelectedPath + "/" + "NewScript.cs";
            NewScriptPath = VFS::MakeUniqueFilePath(NewScriptPath);
            
            FStringView Stem = VFS::FileName(FStringView(NewScriptPath.c_str(), NewScriptPath.size()), true);
            FFixedString ClassName;
            for (char C : Stem)
            {
                const bool bValid = (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_';
                ClassName.push_back(bValid ? C : '_');
            }
            if (ClassName.empty() || (ClassName[0] >= '0' && ClassName[0] <= '9'))
            {
                ClassName.insert(ClassName.begin(), '_');
            }

            FString Contents;
            Contents += "using LuminaSharp;\n";
            Contents += "using Lumina;\n\n";
            Contents += "namespace Game;\n\n";
            Contents += "public sealed class ";
            Contents += ClassName.c_str();
            Contents += " : EntityScript\n";
            Contents += "{\n";
            Contents += "    public override void OnReady()\n";
            Contents += "    {\n";
            Contents += "    }\n\n";
            Contents += "    public override void OnUpdate(float deltaTime)\n";
            Contents += "    {\n";
            Contents += "    }\n";
            Contents += "}\n";

            VFS::WriteFile(NewScriptPath, Contents);
            RefreshContentBrowser();
        }

        if (!bScriptDir && ImGui::MenuItem(LE_ICON_LANGUAGE_CSS3 " New UI Widget"))
        {
            FFixedString NewWidgetPath = SelectedPath + "/" + "NewWidget.rml";
            NewWidgetPath = VFS::MakeUniqueFilePath(NewWidgetPath);
            VFS::WriteFile(NewWidgetPath, "");
            RefreshContentBrowser();
        }

        if (!bScriptDir && ImGui::MenuItem(LE_ICON_LANGUAGE_CSS3 " New UI Stylesheet"))
        {
            FFixedString NewSheetPath = SelectedPath + "/" + "NewStylesheet.rcss";
            NewSheetPath = VFS::MakeUniqueFilePath(NewSheetPath);
            VFS::WriteFile(NewSheetPath,
                "/* New RCSS stylesheet. Link it from a document (relative path):\n"
                "       <link type=\"text/rcss\" href=\"NewStylesheet.rcss\"/>\n"
                "   CPU-safe styling only: gradients (vertical-/horizontal-gradient),\n"
                "   border-radius, transforms, transitions, @keyframes, font-effect.\n"
                "   Avoid box-shadow / filter / linear-gradient (need shaders). */\n\n"
                "body\n"
                "{\n"
                "    color: #cdd6f4;\n"
                "}\n");
            RefreshContentBrowser();
        }

        // IMPORT -----------------------------------------------------------
        // Imports bring in assets, so they're offered only outside Scripts/.
        if (!bScriptDir)
        {
            DrawMenuSection("IMPORT");

            if (ImGui::MenuItem(LE_ICON_IMPORT " Import Asset..."))
            {
                FFixedString SelectedFile;
                const char* Filter = "Supported Assets (*.wav;*.png;*.jpg;*.hdr;*.fbx;*.gltf;*.glb;*.obj;*.ttf;*.otf)\0*.wav;*.png;*.jpg;*.hdr;*.fbx;*.gltf;*.glb;*.obj;*.ttf;*.otf\0All Files (*.*)\0*.*\0";
                if (Platform::OpenFileDialogue(SelectedFile, "Import Asset", Filter))
                {
                    TryImport(SelectedFile);
                }
            }
        }

        // VIEW -------------------------------------------------------------
        DrawMenuSection("VIEW");

        if (ImGui::MenuItem(LE_ICON_REFRESH " Refresh"))
        {
            RefreshContentBrowser();
        }

        if (ImGui::MenuItem(LE_ICON_MICROSOFT_WINDOWS " Show in Explorer"))
        {
            FFixedString Resolved = VFS::ResolvePath(FStringView(SelectedPath.c_str(), SelectedPath.size()));
            const char* Target = Resolved.empty() ? SelectedPath.c_str() : Resolved.c_str();
            Platform::LaunchURL(StringUtils::ToWideString(Target).c_str());
        }
    }
}
