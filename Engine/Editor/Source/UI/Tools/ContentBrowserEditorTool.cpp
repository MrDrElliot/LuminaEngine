#include "ContentBrowserEditorTool.h"
#include "Core/CoreEditorDelegates.h"

#include "EditorAssetActions.h"
#include "EditorToolContext.h"
#include "ReplaceReferencesModal.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetRegistry/TextAssetTypes.h"
#include "Assets/AssetRegistry/TextAssetSidecar.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/Package/Package.h"
#include "Core/Progress/SlowTask.h"
#include "Tools/Import/Importer.h"
#include "UI/Properties/PropertyTable.h"
#include "EASTL/sort.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/ThreadedCallback.h"
#include "Tools/Dialogs/Dialogs.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/EditorColors.h"
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
#include "Thumbnails/AssetTilePainters.h"
#include "Thumbnails/ThumbnailManager.h"
#include "Assets/AssetEvents.h"
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
            constexpr float Gap      = 0.0f;
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

        // "CStaticMesh" -> "Static Mesh". Drops the reflection prefix and splits camel case, so the filter
        // menu reads as asset types rather than as class identifiers.
        FFixedString FriendlyClassName(const FName& ClassName)
        {
            const char* Raw = ClassName.c_str();
            if (Raw == nullptr || Raw[0] == '\0')
            {
                return FFixedString("Unknown");
            }

            // Only a leading 'C' followed by another capital is the reflection prefix; "Curve" keeps its C.
            size_t Start = 0;
            if (Raw[0] == 'C' && Raw[1] >= 'A' && Raw[1] <= 'Z')
            {
                Start = 1;
            }

            FFixedString Out;
            for (size_t i = Start; Raw[i] != '\0'; ++i)
            {
                const char C = Raw[i];
                const bool bBoundary = (i > Start) && (C >= 'A' && C <= 'Z')
                                    && !(Raw[i - 1] >= 'A' && Raw[i - 1] <= 'Z');
                if (bBoundary)
                {
                    Out.push_back(' ');
                }
                Out.push_back(C);
            }
            return Out.empty() ? FFixedString(Raw) : Out;
        }

        // Uppercased, prefix-stripped class name for the tile's type line ("CStaticMesh" -> "STATICMESH").
        FFixedString UpperTypeTag(const FName& ClassName)
        {
            const char* Raw = ClassName.c_str();
            if (Raw == nullptr || Raw[0] == '\0')
            {
                return FFixedString();
            }

            size_t Start = (Raw[0] == 'C' && Raw[1] >= 'A' && Raw[1] <= 'Z') ? 1 : 0;

            FFixedString Out;
            for (size_t i = Start; Raw[i] != '\0'; ++i)
            {
                const char C = Raw[i];
                Out.push_back((C >= 'a' && C <= 'z') ? (char)(C - ('a' - 'A')) : C);
            }
            return Out;
        }

        // Per-type accent + glyph for the fallback tile card. Keyed on the uppercased type tag rather than
        // on CClass, so a type with no factory, no painter and no loaded CDO still gets an identity.
        struct FTypeStyle
        {
            const char* Glyph;
            ImU32       Accent;
        };

        FTypeStyle StyleForType(FStringView TypeTag)
        {
            struct FEntry { const char* Tag; const char* Glyph; ImU32 Accent; };
            static const FEntry kEntries[] =
            {
                { "STATICMESH",       LE_ICON_CUBE_OUTLINE,        IM_COL32(102, 187, 255, 255) },
                { "SKELETALMESH",     LE_ICON_HUMAN,               IM_COL32(129, 199, 255, 255) },
                { "SKELETON",         LE_ICON_BONE,                IM_COL32(190, 210, 235, 255) },
                { "ANIMATION",        LE_ICON_RUN,                 IM_COL32(140, 220, 190, 255) },
                { "ANIMATIONMONTAGE", LE_ICON_ANIMATION_PLAY,      IM_COL32(120, 200, 235, 255) },
                { "TEXTURE",          LE_ICON_IMAGE,               IM_COL32(240, 170, 120, 255) },
                { "TEXTUREARRAY",     LE_ICON_IMAGE_MULTIPLE,      IM_COL32(240, 190, 140, 255) },
                { "MATERIAL",         LE_ICON_SPHERE,              IM_COL32(200, 150, 255, 255) },
                { "MATERIALINSTANCE", LE_ICON_SPHERE,              IM_COL32(180, 140, 235, 255) },
                { "PREFAB",           LE_ICON_CUBE_SCAN,           IM_COL32(255, 205, 110, 255) },
                { "WORLD",            LE_ICON_EARTH,               IM_COL32(120, 210, 150, 255) },
                { "AUDIOSTREAM",      LE_ICON_VOLUME_HIGH,         IM_COL32(255, 150, 170, 255) },
                { "FONT",             LE_ICON_FORMAT_FONT,         IM_COL32(215, 215, 225, 255) },
                { "DATATABLE",        LE_ICON_TABLE,               IM_COL32(150, 200, 220, 255) },
                { "FOLDER",           LE_ICON_FOLDER,              IM_COL32(255, 205, 120, 255) },
                { "CS",               LE_ICON_LANGUAGE_CSHARP,     IM_COL32(160, 200, 130, 255) },
                { "RML",              LE_ICON_XML,                 IM_COL32(200, 180, 140, 255) },
                { "RCSS",             LE_ICON_LANGUAGE_CSS3,       IM_COL32(150, 190, 230, 255) },
            };

            for (const FEntry& Entry : kEntries)
            {
                if (IEquals(TypeTag, Entry.Tag))
                {
                    return { Entry.Glyph, Entry.Accent };
                }
            }
            return { LE_ICON_FILE_DOCUMENT_OUTLINE, IM_COL32(170, 175, 190, 255) };
        }

        // Document card: rounded page with a folded corner, a large type glyph, and the type tag across
        // the bottom. Drawn rather than blitted so it stays sharp at any tile size and needs no atlas.
        void DrawTypeCard(ImDrawList& DrawList, const ImVec2& Min, const ImVec2& Max, FStringView TypeTag)
        {
            const FTypeStyle Style = StyleForType(TypeTag);

            const float Width  = Max.x - Min.x;
            const float Inset  = Math::Max(Width * 0.16f, 4.0f);

            const ImVec2 PageMin(Min.x + Inset, Min.y + Inset * 0.7f);
            const ImVec2 PageMax(Max.x - Inset, Max.y - Inset * 0.7f);
            const float  Fold    = Math::Max((PageMax.x - PageMin.x) * 0.28f, 6.0f);
            const float  Thick   = Math::Max(Width * 0.018f, 1.25f);

            // Page outline with the top-right corner cut away, then the fold drawn over the notch.
            const ImVec2 Outline[] =
            {
                ImVec2(PageMin.x, PageMin.y),
                ImVec2(PageMax.x - Fold, PageMin.y),
                ImVec2(PageMax.x, PageMin.y + Fold),
                ImVec2(PageMax.x, PageMax.y),
                ImVec2(PageMin.x, PageMax.y),
            };

            DrawList.AddConvexPolyFilled(Outline, IM_ARRAYSIZE(Outline), IM_COL32(28, 30, 36, 235));
            DrawList.AddPolyline(Outline, IM_ARRAYSIZE(Outline), Style.Accent, ImDrawFlags_Closed, Thick);

            DrawList.AddLine(ImVec2(PageMax.x - Fold, PageMin.y), ImVec2(PageMax.x - Fold, PageMin.y + Fold), Style.Accent, Thick);
            DrawList.AddLine(ImVec2(PageMax.x - Fold, PageMin.y + Fold), ImVec2(PageMax.x, PageMin.y + Fold), Style.Accent, Thick);

            // Glyph sized off the page, so it tracks the zoom slider instead of snapping between fonts.
            const float GlyphSize = (PageMax.y - PageMin.y) * 0.42f;
            if (GlyphSize >= 8.0f)
            {
                ImFont* Font = ImGui::GetFont();
                const ImVec2 GlyphExtent = Font->CalcTextSizeA(GlyphSize, FLT_MAX, 0.0f, Style.Glyph);
                const ImVec2 GlyphPos((PageMin.x + PageMax.x - GlyphExtent.x) * 0.5f,
                                      PageMin.y + (PageMax.y - PageMin.y) * 0.20f);
                DrawList.AddText(Font, GlyphSize, GlyphPos, Style.Accent, Style.Glyph);
            }

            // Type tag inside the page. Dropped rather than squeezed when the tile is too small for it to
            // be legible -- the line under the name still carries the type.
            const float TagSize = Math::Min((PageMax.y - PageMin.y) * 0.16f, 13.0f);
            if (TagSize >= 7.0f && !TypeTag.empty())
            {
                ImFont* Font = ImGui::GetFont();
                const ImVec2 TagExtent = Font->CalcTextSizeA(TagSize, FLT_MAX, 0.0f, TypeTag.data(), TypeTag.data() + TypeTag.size());
                if (TagExtent.x <= (PageMax.x - PageMin.x) * 0.86f)
                {
                    const ImVec2 TagPos((PageMin.x + PageMax.x - TagExtent.x) * 0.5f,
                                        PageMax.y - TagExtent.y - (PageMax.y - PageMin.y) * 0.10f);
                    DrawList.AddText(Font, TagSize, TagPos, Style.Accent,
                        TypeTag.data(), TypeTag.data() + TypeTag.size());
                }
            }
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

        // Reflector-emitted C# bindings, rewritten by every build and never authored by hand.
        bool IsGeneratedBindingsDirectory(FStringView VirtualPath)
        {
            static constexpr const char* kSuffix = "/Scripts/Generated";
            const size_t Length = FStringView(kSuffix).size();
            return VirtualPath.size() > Length
                && IEquals(VirtualPath.substr(VirtualPath.size() - Length), kSuffix);
        }

        bool ShouldHideDirectory(const VFS::FFileInfo& Info)
        {
            if (Info.IsHidden()) { return true; }
            const FStringView Name(Info.Name.c_str(), Info.Name.size());
            if (!Name.empty() && Name.front() == '.') { return true; }
            if (IsHiddenBrowserDirectory(Name)) { return true; }

            const FStringView VirtualPath(Info.VirtualPath.c_str(), Info.VirtualPath.size());
            if (IsGeneratedBindingsDirectory(VirtualPath)) { return true; }

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

        // Assets belong under a mount's Content directory. "/Game" and "/Engine/Resources" are mount
        // roots that hold Content and Scripts; creating into them puts an asset somewhere the asset
        // registry does not scan.
        bool IsAssetCreationAllowed(FStringView VirtualPath)
        {
            auto IsAtOrUnder = [VirtualPath](const char* RootLiteral)
            {
                const FStringView Root(RootLiteral);
                if (VirtualPath.size() < Root.size() || !IEquals(VirtualPath.substr(0, Root.size()), RootLiteral))
                {
                    return false;
                }
                // A prefix match is only real on a path boundary, or "/Game/ContentPacks" would pass.
                return VirtualPath.size() == Root.size() || VirtualPath[Root.size()] == '/';
            };

            return IsAtOrUnder("/Game/Content") || IsAtOrUnder("/Engine/Resources/Content");
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

    void FContentBrowserEditorTool::NavigateTo(FStringView Path)
    {
        if (Path.empty() || FStringView(SelectedPath.c_str(), SelectedPath.size()) == Path)
        {
            return;
        }

        NavBackStack.push_back(SelectedPath);
        NavForwardStack.clear();

        SelectedPath.assign(Path.data(), Path.size());
        PendingDirectoryReveal = SelectedPath;
        RefreshContentBrowser();
    }

    void FContentBrowserEditorTool::NavigateBack()
    {
        if (NavBackStack.empty())
        {
            return;
        }

        NavForwardStack.push_back(SelectedPath);
        SelectedPath = NavBackStack.back();
        NavBackStack.pop_back();

        PendingDirectoryReveal = SelectedPath;
        RefreshContentBrowser();
    }

    void FContentBrowserEditorTool::NavigateForward()
    {
        if (NavForwardStack.empty())
        {
            return;
        }

        NavBackStack.push_back(SelectedPath);
        SelectedPath = NavForwardStack.back();
        NavForwardStack.pop_back();

        PendingDirectoryReveal = SelectedPath;
        RefreshContentBrowser();
    }

    void FContentBrowserEditorTool::BrowseToAsset(FStringView VirtualPath)
    {
        if (VirtualPath.empty())
        {
            return;
        }

        const FStringView ParentPath = VFS::Parent(VirtualPath, true);
        if (ParentPath.empty())
        {
            return;
        }

        NavigateTo(ParentPath);
        PendingBrowseToPath.assign(VirtualPath.data(), VirtualPath.size());

        // Force a rebuild even when the folder is already the selected one, the tile still has to be found.
        RefreshContentBrowser();
    }

    void FContentBrowserEditorTool::QueueRenameAfterCreate(FStringView VirtualPath)
    {
        if (VirtualPath.empty())
        {
            return;
        }

        PendingRenamePath.assign(VirtualPath.data(), VirtualPath.size());

        // Navigate to the containing folder too: a factory can be invoked from a path other than the
        // one on screen, and a rename box on a tile the user cannot see would just eat their keystrokes.
        const FStringView ParentPath = VFS::Parent(VirtualPath, true);
        if (!ParentPath.empty())
        {
            SelectedPath.assign(ParentPath.data(), ParentPath.size());
            PendingDirectoryReveal = SelectedPath;
        }

        RefreshContentBrowser();
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

        RefreshFilterClasses();

        ContentBrowserTileViewContext.bShowTypeLabels = true;

        CreateToolWindow("Content", [&] (bool bIsFocused)
        {
            // Starting width only. The directory pane is resizable, so its actual width is whatever
            // the user last dragged it to.
            constexpr float DefaultDirectoryWidth = 225.0f;

            DrawDirectoryBrowser(bIsFocused, ImVec2(DefaultDirectoryWidth, 0));

            ImGui::SameLine();

            // Measured, not derived from the default: subtracting a constant would ignore the drag and
            // leave the content pane overlapping or short by however far the splitter had moved.
            DrawContentBrowser(bIsFocused, ImVec2(ImGui::GetContentRegionAvail().x, 0));
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

            // Set when this asset's type draws its own tile body (see FAssetTilePainterRegistry). The
            // button below still draws its frame and owns every interaction; only the image is replaced.
            FAssetTilePainterFn* TilePainter = nullptr;
            CObject*             PainterAsset = nullptr;

            // Set when nothing else supplies a picture: the tile draws a type card instead of an image.
            bool bDrawTypeCard = true;

            ImTextureRef ImTexture;
            switch (ContentItem->GetIconKind())
            {
            case EIconKind::Directory:
                {
                    // Folders keep the folder image: navigation should not read as a document.
                    ImTexture = ImGuiX::ToImTextureRef(Paths::GetEngineResourceDirectory() + "/Textures/Folder.png");
                    TintColor = ImVec4(1.0f, 0.82f, 0.45f, 1.0f);
                    bDrawTypeCard = false;
                    break;
                }
            case EIconKind::Asset:
                {
                    // A type-specific painter takes precedence over the rendered thumbnail: it is sharp at
                    // any tile size and reflects edits immediately, with no cache to invalidate.
                    //
                    // RESIDENT ONLY. FindObject is a hash lookup that never loads -- loading here would race
                    // the editor's own loader on a non-atomic object (the rule CThumbnailManager's render
                    // queue documents). An asset nothing has loaded yet just keeps its ordinary thumbnail.
                    FStringView AssetPath = ContentItem->GetVirtualPath();
                    if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(AssetPath))
                    {
                        if (CObject* Resident = FindObject<CObject>(Data->AssetGUID))
                        {
                            TilePainter = FAssetTilePainterRegistry::Get().Find(Resident->GetClass());
                            if (TilePainter != nullptr)
                            {
                                PainterAsset = Resident;
                            }
                        }
                    }

                    // A rendered thumbnail is the asset's own likeness and always beats a generic card.
                    if (FPackageThumbnail* MaybeThumbnail = CThumbnailManager::Get().GetThumbnailForPackage(ContentItem->GetVirtualPath()))
                    {
                        ImTexture = ImGuiX::ToImTextureRef(MaybeThumbnail->LoadedImage);
                        bDrawTypeCard = false;
                    }
                    break;
                }
            default:
                break;
            }

            // A painter owns the whole body; the card would draw underneath it.
            if (TilePainter != nullptr)
            {
                bDrawTypeCard = false;
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
            
            // A painted tile hides the image (alpha 0) rather than skipping ImageButton: the button still
            // supplies the frame, hover/active styling, sizing and the click handling below, so the painter
            // only has to fill the body.
            // The button always supplies the frame, hover/active styling, sizing and click handling; when
            // a painter or a type card fills the body the image itself is drawn fully transparent.
            const bool bHideImage = (TilePainter != nullptr) || bDrawTypeCard;
            ImGui::ImageButton("##", ImTexture, Size, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                bHideImage ? ImVec4(TintColor.x, TintColor.y, TintColor.z, 0.0f) : TintColor);

            // Item rect, not Pos/Size: it already accounts for the frame padding pushed above, so the
            // body bounds cannot drift if that padding is ever retuned.
            const ImVec2 BodyMin = ImGui::GetItemRectMin();
            const ImVec2 BodyMax = ImGui::GetItemRectMax();

            if (TilePainter != nullptr)
            {
                (*TilePainter)(PainterAsset, *DrawList, BodyMin, BodyMax);
            }
            else if (bDrawTypeCard)
            {
                DrawTypeCard(*DrawList, BodyMin, BodyMax, ContentItem->GetTypeLabel());
            }

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
                // Shift beats Ctrl when both are held, matching every file manager.
                if (ImGui::GetIO().KeyShift)
                {
                    return FTileViewItem::EClickState::SingleWithShift;
                }

                if (ImGui::GetIO().KeyCtrl)
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
                NavigateTo(FStringView(Path.c_str(), Path.size()));
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
            else if (ContentItem->GetPathSource().ends_with(".cs"))
            {
                // Scripts honor the editor choice in CScriptEditorSettings.
                ToolContext->OpenScriptEditor(ContentItem->GetPathSource());
            }
            else
            {
                // No in-engine editor: hand the native file to the OS association.
                Platform::LaunchURL(UTF8_TO_TCHAR(ContentItem->GetPathSource().data()));
            }
        };

        ContentBrowserTileViewContext.ItemSelectedFunction = [this] (FTileViewItem* Item)
        {
            
        };
        
        ContentBrowserTileViewContext.DrawItemContextMenuFunction = [this] (const TVector<FTileViewItem*>& Items)
        {
            if (Items.empty())
            {
                return;
            }

            if (Items.size() == 1)
            {
                DrawAssetContextMenu(static_cast<FContentBrowserTileViewItem*>(Items[0]));
                return;
            }

            DrawMultiSelectionContextMenu(Items);
        };

        ContentBrowserTileViewContext.RebuildTreeFunction = [this] (FTileViewWidget* Tree)
        {
            // The Filter menu toggles asset classes. Directories, scripts, and loose files are not
            // class-filterable, so they are always shown; assets are hidden when their class is off.
            // Type label resolved ONCE per entry and carried to the tile. It costs a registry lookup and
            // a string build, and both the filter test and the tile need it.
            struct FBrowseEntry
            {
                VFS::FFileInfo Info;
                FFixedString   TypeLabel;
            };

            TVector<FBrowseEntry> SortedPaths;

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
                    if (!IsBrowsableFileExtension(FileInfo.GetExt()))
                    {
                        return;
                    }
                }

                FFixedString TypeLabel = MakeTypeLabel(FileInfo);
                if (!PassesFilters(FileInfo, FStringView(TypeLabel.c_str(), TypeLabel.size())))
                {
                    return;
                }

                SortedPaths.push_back(FBrowseEntry{ FileInfo, Move(TypeLabel) });
            });
            
            eastl::sort(SortedPaths.begin(), SortedPaths.end(), [&](const FBrowseEntry& LHS, const FBrowseEntry& RHS)
            {
                if (LHS.Info.IsDirectory() != RHS.Info.IsDirectory())
                {
                    return LHS.Info.IsDirectory();
                }

                return LHS.Info.Name < RHS.Info.Name;
            });
            
            // Extension-insensitive so a package path ("/Game/Foo.lasset") still matches however the
            // VFS spelled the entry.
            const FStringView BrowseTarget = VFS::RemoveExtension(FStringView(PendingBrowseToPath.c_str(), PendingBrowseToPath.size()));
            const FStringView RenameTarget = VFS::RemoveExtension(FStringView(PendingRenamePath.c_str(), PendingRenamePath.size()));
            FTileViewItem*               BrowseItem = nullptr;
            FContentBrowserTileViewItem* RenameItem = nullptr;

            for (const FBrowseEntry& Entry : SortedPaths)
            {
                const VFS::FFileInfo& Info = Entry.Info;
                const bool bProtected = IsProtectedRoot(FStringView(Info.VirtualPath.c_str(), Info.VirtualPath.size()));
                FContentBrowserTileViewItem* NewItem = ContentBrowserTileView.AddItemToTree<FContentBrowserTileViewItem>(nullptr, Info, bProtected);
                NewItem->SetTypeLabel(Entry.TypeLabel);

                const FStringView ItemPath = VFS::RemoveExtension(NewItem->GetVirtualPath());
                if (!BrowseTarget.empty() && ItemPath == BrowseTarget)
                {
                    BrowseItem = NewItem;
                }
                if (!RenameTarget.empty() && ItemPath == RenameTarget)
                {
                    RenameItem = NewItem;
                }
            }

            PendingBrowseToPath.clear();

            if (BrowseItem != nullptr)
            {
                Tree->SelectAndScrollTo(BrowseItem);
            }

            // Rename-on-create. Held across rebuilds until the tile actually shows up, unlike the browse
            // target above: a factory with a creation dialogue finishes its work on a task thread, so the
            // first rebuild after the request routinely runs before the file has landed on disk.
            if (RenameItem != nullptr)
            {
                PendingRenamePath.clear();
                Tree->SelectAndScrollTo(RenameItem);
                ContentBrowserTileView.BeginInlineRename(RenameItem);
            }
        };

        ContentBrowserTileViewContext.KeyPressedFunction = [this] (FTileViewItem& Item, ImGuiKey Key) -> bool
        {
            if (Key == ImGuiKey_F2)
            {
                FContentBrowserTileViewItem* ContentItem = static_cast<FContentBrowserTileViewItem*>(&Item);
                if (ContentItem->IsProtected())
                {
                    ImGuiX::Notifications::NotifyError("Cannot rename a core directory");
                    return true;
                }

                ContentBrowserTileView.BeginInlineRename(ContentItem);
                return true;
            }

            if (Key == ImGuiKey_Delete)
            {
                // Item is only the key's anchor (the first selection). Delete has to take the whole set,
                // or a marquee over fifty assets removes exactly one.
                const TVector<FTileViewItem*>& Selections = ContentBrowserTileView.GetSelections();
                if (Selections.size() > 1)
                {
                    DeleteSelectedItems(Selections);
                    return true;
                }

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

        ContentBrowserTileViewContext.ItemRenamedFunction = [this] (FTileViewItem* Item, const char* NewName)
        {
            FContentBrowserTileViewItem* ContentItem = static_cast<FContentBrowserTileViewItem*>(Item);

            // Empty or unchanged: just drop the edit, no error spam.
            if (NewName == nullptr || NewName[0] == 0)
            {
                return;
            }

            if (ContentItem->GetName() == FStringView(NewName))
            {
                return;
            }

            FStringView PathNoExt = VFS::RemoveExtension(ContentItem->GetVirtualPath());
            FFixedString TestPath = Paths::Combine(VFS::Parent(PathNoExt), NewName);
            TestPath.append_convert(ContentItem->GetExtension());

            if (VFS::Exists(TestPath))
            {
                ImGuiX::Notifications::NotifyError("Rename Failed, path already exists: {0}", TestPath);
                return;
            }

            ActionRegistry.EnqueueAction<FPendingRename>(FPendingRename{ FFixedString(ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().length()), TestPath });
        };

        // The tree selects the node on right-click before opening this, so SelectedPath is already the
        // folder under the cursor and the shared directory menu targets the right thing.
        DirectoryContext.ItemContextMenuFunction = [this](FTreeListView&, FTreeNodeID)
        {
            PushContextMenuItemStyle();
            DrawContentDirectoryContextMenu();
            PopContextMenuItemStyle();
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

            if (FStringView(Info.VirtualPath.c_str(), Info.VirtualPath.size()) == FStringView(SelectedPath.c_str(), SelectedPath.size()))
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

            // No-ops when the tree is only catching up to a navigation that already happened, which is
            // what keeps RevealPendingDirectory from recording history of its own.
            NavigateTo(FStringView(Data.Path.c_str(), Data.Path.size()));
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
                        TextPaths.emplace_back(FileInfo.VirtualPath.c_str(), FileInfo.VirtualPath.size());
                    }
                });

                VFS::RemoveAll(Destroy.PendingDestroy);

                for (const FFixedString& Tp : TextPaths)
                {
                    FAssetRegistry::Get().TextAssetDeleted(FStringView(Tp.c_str(), Tp.size()));
                    FCoreEditorDelegates::OnAssetDeleted.Broadcast(FStringView(Tp.c_str(), Tp.size()));
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
                    FCoreEditorDelegates::OnAssetDeleted.Broadcast(Destroy.PendingDestroy);
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
                    FCoreEditorDelegates::OnAssetDeleted.Broadcast(Destroy.PendingDestroy);
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
                FCoreEditorDelegates::OnAssetRenamed.Broadcast(Rename.OldName, Rename.NewName);
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
                    FCoreEditorDelegates::OnAssetRenamed.Broadcast(Entry.OldPath, Entry.NewPath);
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
                    FCoreEditorDelegates::OnAssetRenamed.Broadcast(Rename.OldName, Rename.NewName);
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
            "F2 renames the tile in place; Delete removes. References follow a rename by asset GUID, "
            "so inbound references stay intact without redirectors.");
    }

    FFixedString FContentBrowserEditorTool::MakeTypeLabel(const VFS::FFileInfo& FileInfo)
    {
        if (FileInfo.IsDirectory())
        {
            return FFixedString("FOLDER");
        }

        if (FileInfo.IsLAsset())
        {
            const FStringView Path(FileInfo.VirtualPath.c_str(), FileInfo.VirtualPath.size());
            if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Path))
            {
                return UpperTypeTag(Data->AssetClass);
            }
            // Discovered on disk but not registered yet; the tile still needs a stable line.
            return FFixedString("ASSET");
        }

        // Loose file: its extension, minus the dot.
        const FString Ext = FileInfo.GetExt();
        FFixedString Out;
        for (size_t i = (!Ext.empty() && Ext[0] == '.') ? 1 : 0; i < Ext.size(); ++i)
        {
            const char C = Ext[i];
            Out.push_back((C >= 'a' && C <= 'z') ? (char)(C - ('a' - 'A')) : C);
        }
        return Out;
    }

    void FContentBrowserEditorTool::RefreshFilterClasses()
    {
        // Driven off what the registry actually holds, not off the factories: a type with no factory
        // (meshes, skeletons, animations, prefabs) is still something you want to filter by, and seeding
        // from factories silently omitted them -- a checkbox that was never there cannot be unticked, and
        // the missing entry defaulted to "show", so those types ignored the filter entirely.
        for (const TUniquePtr<FAssetData>& Data : FAssetRegistry::Get().GetAssets())
        {
            if (Data && !Data->AssetClass.IsNone() && FilterState.find(Data->AssetClass) == FilterState.end())
            {
                // Existing choices win; only genuinely new types default to visible.
                FilterState.emplace(Data->AssetClass, true);
            }
        }
    }

    bool FContentBrowserEditorTool::PassesFilters(const VFS::FFileInfo& FileInfo, FStringView TypeLabel) const
    {
        // Folders are navigation, not content: they stay put under a type filter, and only disappear when
        // a search is active and their name does not match.
        const bool bDirectory = FileInfo.IsDirectory();

        if (!bDirectory && FileInfo.IsLAsset())
        {
            const FStringView Path(FileInfo.VirtualPath.c_str(), FileInfo.VirtualPath.size());
            if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Path))
            {
                auto It = FilterState.find(Data->AssetClass);
                if (It != FilterState.end() && !It->second)
                {
                    return false;
                }
            }
        }

        if (SearchText.empty())
        {
            return true;
        }

        const FStringView Search(SearchText.c_str(), SearchText.size());
        const FStringView Name = VFS::FileName(FileInfo.PathSource, true);

        // Type is searchable too, so "texture" narrows to textures without opening the filter menu.
        return ImGuiX::PassSearchFilter(Search, Name) || (!bDirectory && ImGuiX::PassSearchFilter(Search, TypeLabel));
    }

    void FContentBrowserEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        // Search first: it is the control reached for most often, and it reads as part of the path bar.
        ImGui::SetNextItemWidth(200.0f);
        {
            char Buffer[128];
            const size_t Copied = Math::Min(SearchText.size(), sizeof(Buffer) - 1);
            memcpy(Buffer, SearchText.c_str(), Copied);
            Buffer[Copied] = '\0';

            if (ImGui::InputTextWithHint("##ContentSearch", LE_ICON_MAGNIFY " Search...", Buffer, sizeof(Buffer)))
            {
                SearchText = Buffer;
                RefreshContentBrowser();
            }
        }

        if (!SearchText.empty())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(LE_ICON_CLOSE "##ClearSearch"))
            {
                SearchText.clear();
                RefreshContentBrowser();
            }
        }

        // Count of hidden types, so a filter left on is visible from the menu bar rather than being
        // discovered later as "my assets disappeared".
        uint32 HiddenTypes = 0;
        for (const auto& [Name, State] : FilterState)
        {
            HiddenTypes += State ? 0u : 1u;
        }

        FFixedString FilterLabel(FFixedString::CtorSprintf(),
            HiddenTypes > 0 ? LE_ICON_FILTER " Filter (%u hidden)###Filter" : LE_ICON_FILTER " Filter###Filter", HiddenTypes);

        if (HiddenTypes > 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::AccentAlt());
        }
        const bool bFilterOpen = ImGui::BeginMenu(FilterLabel.c_str());
        if (HiddenTypes > 0)
        {
            ImGui::PopStyleColor();
        }

        if (bFilterOpen)
        {
            // Only while the menu is open: a missing entry means "shown", so the map has to be complete
            // when it is DRAWN, not on every refresh.
            RefreshFilterClasses();

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 2));

            if (ImGui::SmallButton("Show All"))
            {
                for (auto& [Name, State] : FilterState) { State = true; }
                RefreshContentBrowser();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Hide All"))
            {
                for (auto& [Name, State] : FilterState) { State = false; }
                RefreshContentBrowser();
            }

            ImGui::Separator();

            if (FilterState.empty())
            {
                ImGui::TextDisabled("No assets discovered yet.");
            }

            // Alphabetical: the map's bucket order is arbitrary and would reshuffle the menu whenever a
            // new type is discovered.
            TVector<FName> Names;
            Names.reserve(FilterState.size());
            for (const auto& [Name, State] : FilterState)
            {
                Names.push_back(Name);
            }
            eastl::sort(Names.begin(), Names.end(), [](const FName& A, const FName& B)
            {
                return strcmp(A.c_str(), B.c_str()) < 0;
            });

            for (const FName& Name : Names)
            {
                bool& State = FilterState[Name];
                if (ImGui::Checkbox(FriendlyClassName(Name).c_str(), &State))
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

    void FContentBrowserEditorTool::OpenDeletionWarningPopup(const FContentBrowserTileViewItem* Item)
    {
        TVector<FFixedString> Paths;
        Paths.emplace_back(Item->GetVirtualPath().data(), Item->GetVirtualPath().size());

        RequestDeletion(Move(Paths), 0);
    }

    FFixedString FContentBrowserEditorTool::FindBlockingPrefabVariants(const TVector<FFixedString>& Paths) const
    {
        FFixedString DependentVariants;

        for (const FFixedString& Path : Paths)
        {
            const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Path);
            if (Data == nullptr)
            {
                continue;
            }

            CClass* AssetClass = FindObject<CClass>(Data->AssetClass);
            if (AssetClass == nullptr || !AssetClass->IsChildOf(CPrefab::StaticClass()))
            {
                continue;
            }

            CPrefab* Prefab = LoadObject<CPrefab>(Data->AssetGUID);
            if (Prefab == nullptr)
            {
                continue;
            }

            for (CPrefab* Variant : Prefab->FindDirectVariants())
            {
                if (!DependentVariants.empty())
                {
                    DependentVariants += ", ";
                }
                DependentVariants += Variant->GetName().c_str();
            }
        }

        return DependentVariants;
    }

    void FContentBrowserEditorTool::OpenReplaceReferencesModal(const FContentBrowserTileViewItem* Item)
    {
        TVector<FFixedString> Paths;
        Paths.emplace_back(Item->GetVirtualPath().data(), Item->GetVirtualPath().size());

        TVector<FAssetReferenceFixup> Plan = ReplaceReferences::BuildPlan(Paths);
        if (Plan.empty())
        {
            ImGuiX::Notifications::NotifyInfo("Nothing references '{0}'", Item->GetName());
            return;
        }

        ReplaceReferences::OpenModal(ToolContext, Move(Plan), EReferenceFixupMode::Standalone, 1, nullptr);
    }

    void FContentBrowserEditorTool::RequestDeletion(TVector<FFixedString> Paths, int32 ProtectedCount)
    {
        if (Paths.empty())
        {
            ImGuiX::Notifications::NotifyError("Nothing to delete, every selected entry is protected");
            return;
        }

        if (IsAnyWorldPlayingOrSimulating())
        {
            ImGuiX::Notifications::NotifyError("Stop play before deleting content");
            return;
        }

        // Refused rather than cascaded: a variant is defined BY its parent, so deleting it empties descendants.
        const FFixedString DependentVariants = FindBlockingPrefabVariants(Paths);
        if (!DependentVariants.empty())
        {
            ImGuiX::Notifications::NotifyError("A selected prefab is the parent of {0}. Delete or reparent those first.",
                DependentVariants);
            return;
        }

        auto Commit = [this, Paths, ProtectedCount]()
        {
            for (const FFixedString& Path : Paths)
            {
                ActionRegistry.EnqueueAction<FPendingDestroy>(FPendingDestroy{ Path });
            }

            if (ProtectedCount > 0)
            {
                ImGuiX::Notifications::NotifyWarning("Skipped {0} protected entries", ProtectedCount);
            }

            // Queued destruction kills the tiles, so a held selection pointer is a crash on the next draw.
            ContentBrowserTileView.ClearSelections();
        };

        TVector<FAssetReferenceFixup> Plan = ReplaceReferences::BuildPlan(Paths);
        if (!Plan.empty())
        {
            ReplaceReferences::OpenModal(ToolContext, Move(Plan), EReferenceFixupMode::BeforeDelete, (uint32)Paths.size(),
                [Commit = Move(Commit)](bool bProceed)
                {
                    if (bProceed)
                    {
                        Commit();
                    }
                });
            return;
        }

        // Nothing points at these, so the plain confirmation is the whole safety story.
        if (Paths.size() == 1)
        {
            const FStringView Name = VFS::FileName(FStringView(Paths[0].c_str(), Paths[0].size()), true);

            if (!VFS::IsEmpty(Paths[0]) &&
                !Dialogs::Confirmation("Confirm Deletion", "Are you sure you want to delete \"{0}\"?\n""\nThis action cannot be undone.", Name))
            {
                return;
            }
        }
        else if (!Dialogs::Confirmation("Confirm Deletion",
            "Are you sure you want to delete {0} selected item(s)?\n""\nThis action cannot be undone.", Paths.size()))
        {
            return;
        }

        Commit();
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
        NavBackStack.clear();
        NavForwardStack.clear();
        RefreshContentBrowser();
    }

    bool FContentBrowserEditorTool::DrawImportWindow(
        CImporter* Importer,
        const FImportRequest& Request,
        int32 RemainingCount,
        bool& bShouldClose,
        bool& bOutApplyToAll)
    {
        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::LargeBold);
        ImGuiX::TextColoredUnformatted(EditorColors::Accent(), LE_ICON_IMPORT);
        ImGui::SameLine();
        ImGui::TextUnformatted(VFS::FileName(Request.SourcePath).data());
        ImGui::PopFont();

        ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextDim());
        ImGui::TextUnformatted(Request.DestinationPath.c_str());
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Settings scroll; the footer stays pinned so the buttons never walk off a long options list.
        const float FooterHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

        if (ImGui::BeginChild("##ImportSettings", ImVec2(0.0f, -FooterHeight), ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            // Options come straight off the importer's reflected properties, so a new importer gets its
            // settings UI for free.
            if (ImportSettingsTable)
            {
                ImportSettingsTable->DrawTree();
            }

            ImGui::Spacing();
            Importer->DrawSourcePreview();
        }
        ImGui::EndChild();

        ImGui::PopStyleVar(4);

        ImGui::Separator();
        ImGui::Spacing();

        constexpr float ButtonWidth = 110.0f;
        const bool bHasMore = RemainingCount > 0;

        if (bHasMore)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextDim());
            ImGui::Text("%d more queued", RemainingCount);
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }

        const float ButtonCount = bHasMore ? 3.0f : 2.0f;
        const float ButtonsWidth = ButtonWidth * ButtonCount + ImGui::GetStyle().ItemSpacing.x * (ButtonCount - 1.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ButtonsWidth);

        // The chosen values become the defaults for the next import of this type, which is what a batch
        // wants and what the old per-factory static used to give.
        auto CommitSettings = [Importer]()
        {
            if (CImporter* CDO = Importer->GetClass()->GetDefaultObject<CImporter>())
            {
                Importer->CopyPropertiesTo(CDO);
            }
        };

        bool bConfirmed = false;
        if (ImGui::Button("Import", ImVec2(ButtonWidth, 0.0f)))
        {
            CommitSettings();
            bConfirmed = true;
            bShouldClose = true;
        }

        if (bHasMore)
        {
            ImGui::SameLine();

            // These settings become the answer for everything still queued, so the rest import
            // without asking again.
            if (ImGui::Button("Import All", ImVec2(ButtonWidth, 0.0f)))
            {
                CommitSettings();
                bConfirmed = true;
                bShouldClose = true;
                bOutApplyToAll = true;
            }
        }

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, EditorColors::Danger());
        if (ImGui::Button("Cancel", ImVec2(ButtonWidth, 0.0f)))
        {
            bShouldClose = true;
        }
        ImGui::PopStyleColor();

        return bConfirmed;
    }

    FFixedString FContentBrowserEditorTool::MakeUniqueImportDestination(FStringView SourcePath)
    {
        // Stem only. An import becomes a package, and a package name is the path with any extension
        // stripped (see SanitizeObjectName), so "Foo.png" and "Foo.jpg" both want to be "Foo".
        const FFixedString Base = Paths::Combine(SelectedPath, VFS::FileName(SourcePath, true));

        // Three namespaces have to agree. Testing only the source path on disk -- which is what this used
        // to do -- tests a name that can never exist, since the asset lands as "<stem>.lasset".
        auto IsFree = [this](const FFixedString& Candidate) -> bool
        {
            // What CreatePackage actually refuses on.
            if (FindObject<CPackage>(Candidate) != nullptr)
            {
                return false;
            }

            // An asset already on disk that nothing has loaded this session.
            FFixedString OnDisk = Candidate;
            CPackage::AddPackageExt(OnDisk);
            if (VFS::Exists(OnDisk))
            {
                return false;
            }

            return ReservedImportPaths.find(Candidate) == ReservedImportPaths.end();
        };

        if (IsFree(Base))
        {
            ReservedImportPaths.insert(Base);
            return Base;
        }

        for (uint32 N = 1; N < 10000; ++N)
        {
            FFixedString Candidate = Base;
            Candidate.append("_").append_convert(eastl::to_string(N).c_str());

            if (IsFree(Candidate))
            {
                ReservedImportPaths.insert(Candidate);
                return Candidate;
            }
        }

        return {};
    }

    void FContentBrowserEditorTool::StartImport(CImporter* Importer, const FImportRequest& Request)
    {
        Task::AsyncTask(1, 1, [this, Importer, Request](uint32, uint32, uint32)
        {
            const FStringView SourceName = VFS::FileName(Request.SourcePath, true);
            FFixedString Title(FFixedString::CtorSprintf(), "Importing %.*s", (int)SourceName.length(), SourceName.data());
            FScopedSlowTask SlowTask(1.0f, Title, "Processing...");

            FImportResult Result;
            Importer->BuildAssets(Request, Result, &SlowTask);

            // Reverse order: a generated graph must be destroyed before the material it back-references,
            // and the scene prefab before the meshes it holds references to.
            for (auto It = Result.CreatedObjects.rbegin(); It != Result.CreatedObjects.rend(); ++It)
            {
                (*It)->ConditionalBeginDestroy();
            }

            const bool bSucceeded = Result.Succeeded();
            const FString Error   = Result.Error;

            MainThread::Enqueue([this, Importer, Request, bSucceeded, Error]()
            {
                CImporterRegistry::DestroyImporter(Importer);

                // Released only now the import has run. On success the package is registered and saved, so
                // the checks above see it; on failure the name goes back to being free.
                ReservedImportPaths.erase(Request.DestinationPath);

                RefreshContentBrowser();
                if (bSucceeded)
                {
                    ImGuiX::Notifications::NotifySuccess("Successfully Imported: \"{0}\"", Request.SourcePath);
                }
                else
                {
                    ImGuiX::Notifications::NotifyError("Failed to import \"{0}\": {1}", Request.SourcePath, Error);
                }
            });
        });
    }

    void FContentBrowserEditorTool::TryImport(const FFixedString& Path)
    {
        TVector<FFixedString> Single;
        Single.push_back(Path);
        TryImport(Single);
    }

    void FContentBrowserEditorTool::TryImport(const TVector<FFixedString>& Paths)
    {
        // Anything whose importer has no options dialogue imports straight away -- textures, fonts, audio.
        // Only importers that ask something go in the queue, so dropping forty PNGs and one FBX is one
        // prompt, not forty-one.
        for (const FFixedString& Path : Paths)
        {
            CImporter* ImporterCDO = CImporterRegistry::Get().FindImporterForExtension(VFS::Extension(Path));
            if (ImporterCDO == nullptr)
            {
                ImGuiX::Notifications::NotifyWarning("No importer for \"{0}\"", VFS::FileName(Path).data());
                continue;
            }

            if (!ImporterCDO->HasSettingsDialogue())
            {
                const FFixedString Destination = MakeUniqueImportDestination(Path);
                if (Destination.empty())
                {
                    ImGuiX::Notifications::NotifyError("No free asset name for \"{0}\"", VFS::FileName(Path).data());
                    continue;
                }

                CImporter* Importer = CImporterRegistry::CreateImporterOfClass(ImporterCDO->GetClass());
                StartImport(Importer, FImportRequest{ Path, Destination });
                continue;
            }

            // Queued files get their destination when they reach the front, not here: the dialogue can sit
            // open for minutes, and reserving forty names up front would make every later one a "_1".
            PendingImports.push_back(Path);
        }

        // A queue already running will pick these up when it advances.
        if (!bImportWindowOpen)
        {
            ProcessNextImport();
        }
    }

    void FContentBrowserEditorTool::ProcessNextImport()
    {
        if (PendingImports.empty())
        {
            // The batch is over, so the next one starts by asking again.
            bApplyImportSettingsToAll = false;
            return;
        }

        const FFixedString Path = PendingImports.front();
        PendingImports.erase(PendingImports.begin());

        CImporter* ImporterCDO = CImporterRegistry::Get().FindImporterForExtension(VFS::Extension(Path));
        if (ImporterCDO == nullptr)
        {
            ProcessNextImport();
            return;
        }

        const FFixedString DestinationPath = MakeUniqueImportDestination(Path);
        if (DestinationPath.empty())
        {
            ImGuiX::Notifications::NotifyError("No free asset name for \"{0}\"", VFS::FileName(Path).data());
            ProcessNextImport();
            return;
        }

        bImportWindowOpen = true;

        const FImportRequest Request{ Path, DestinationPath };
        CImporter* Importer = CImporterRegistry::CreateImporterOfClass(ImporterCDO->GetClass());

        // Each file is parsed on its own: Import All reuses the chosen OPTIONS, not the previous file's
        // parsed contents, which is why the parse still runs for every one of them.
        Task::AsyncTask(1, 1, [this, Importer, Request](uint32, uint32, uint32)
        {
            const FStringView SourceName = VFS::FileName(Request.SourcePath, true);
            FFixedString Title(FFixedString::CtorSprintf(), "Reading %.*s", (int)SourceName.length(), SourceName.data());
            FScopedSlowTask SlowTask(1.0f, Title, "Parsing source file...");

            FString Error;
            const bool bParsed = Importer->ParseSource(Request, Error, &SlowTask);

            MainThread::Enqueue([this, Importer, Request, bParsed, Error]() mutable
            {
                if (!bParsed)
                {
                    ImGuiX::Notifications::NotifyError("Failed to import \"{0}\": {1}", Request.SourcePath, Error);
                    CImporterRegistry::DestroyImporter(Importer);
                    // Nothing will be created at the reserved name, so hand it back.
                    ReservedImportPaths.erase(Request.DestinationPath);
                    bImportWindowOpen = false;
                    ProcessNextImport();
                    return;
                }

                if (bApplyImportSettingsToAll)
                {
                    StartImport(Importer, Request);
                    bImportWindowOpen = false;
                    ProcessNextImport();
                    return;
                }

                // Only once the dialogue is actually going to show them: the preview mints a GPU texture
                // per source image, which is wasted work and live GPU state for an import that never asks.
                Importer->PrepareSettingsPreview();

                ImportSettingsTable = MakeUnique<FPropertyTable>(Importer);
                ImportSettingsTable->SetShowSearchBar(false);

                struct FModalState
                {
                    bool bShouldClose = false;
                    bool bStarted     = false;
                };
                auto SharedState = MakeShared<FModalState>();

                ToolContext->PushModal("Import", {940, 900},
                    [this, Importer, Request, SharedState]() mutable
                    {
                        bool bApplyToAll = false;

                        if (DrawImportWindow(Importer, Request, (int32)PendingImports.size(),
                                             SharedState->bShouldClose, bApplyToAll))
                        {
                            bApplyImportSettingsToAll = bApplyToAll;
                            SharedState->bStarted = true;
                            StartImport(Importer, Request);
                        }

                        if (SharedState->bShouldClose)
                        {
                            ImportSettingsTable.reset();

                            // Cancelled: StartImport never ran, so nothing else releases the name or the
                            // importer.
                            if (!SharedState->bStarted)
                            {
                                CImporterRegistry::DestroyImporter(Importer);
                                ReservedImportPaths.erase(Request.DestinationPath);
                            }

                            // Advancing from here rather than from the confirm branch, so cancelling one
                            // file skips it and moves on instead of abandoning the rest of the batch.
                            bImportWindowOpen = false;
                            MainThread::Enqueue([this]() { ProcessNextImport(); });
                        }

                        return SharedState->bShouldClose;
                    });
            });
        });
    }

    void FContentBrowserEditorTool::DrawDirectoryBrowser(bool bIsFocused, ImVec2 Size)
    {
        // ResizeX gives the pane a draggable right border. It also turns the size argument into a
        // first-use default and persists the user's width in the ini, so the caller must not compute
        // the content pane's width from that default -- see DrawContentBrowser's call site.
        ImGui::BeginChild("Directories", Size, ImGuiChildFlags_ResizeX, ImGuiWindowFlags_HorizontalScrollbar);

        // Wraps the tree because the widget owns the BeginPopup for a right-clicked node; pushing here
        // is the only way that menu picks up the same styling as the one in the tile pane.
        PushContextMenuWindowStyle();

        DirectoryListView.Draw(DirectoryContext);

        // Empty space below the tree acts on the current folder, matching the tile pane's background menu.
        if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        {
            ImGui::OpenPopup("DirectoryPaneContextMenu");
            ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f, 0.0f), ImVec2(360.0f, FLT_MAX));
        }

        if (ImGui::BeginPopup("DirectoryPaneContextMenu"))
        {
            PushContextMenuItemStyle();
            DrawContentDirectoryContextMenu();
            PopContextMenuItemStyle();

            ImGui::EndPopup();
        }

        PopContextMenuWindowStyle();

        // After Draw, so the tree has been rebuilt and the nodes we walk actually exist. The scroll
        // request lands on the next Draw.
        RevealPendingDirectory();

        ImGui::EndChild();
    }

    void FContentBrowserEditorTool::RevealPendingDirectory()
    {
        if (PendingDirectoryReveal.empty() || DirectoryListView.IsDirty())
        {
            return;
        }

        const FStringView Target(PendingDirectoryReveal.c_str(), PendingDirectoryReveal.size());
        PendingDirectoryReveal.clear();

        // True when NodePath is Target itself or one of its ancestors.
        auto LeadsToTarget = [Target](FStringView NodePath)
        {
            if (NodePath.empty() || !Target.starts_with(NodePath))
            {
                return false;
            }

            return Target.size() == NodePath.size() || Target[NodePath.size()] == '/';
        };

        // Node paths are full virtual paths, so a prefix walk finds the branch without segment math.
        auto FindChildLeadingToTarget = [&](FTreeNodeID Parent)
        {
            const int32 NumChildren = DirectoryListView.NumChildNodes(Parent);
            for (int32 Index = 0; Index < NumChildren; ++Index)
            {
                const FTreeNodeID Child = DirectoryListView.GetChildNode(Parent, Index);
                const FContentBrowserListViewItemData& Data = DirectoryListView.Get<FContentBrowserListViewItemData>(Child);
                if (LeadsToTarget(FStringView(Data.Path.c_str(), Data.Path.size())))
                {
                    return Child;
                }
            }

            return InvalidTreeNode;
        };

        FTreeNodeID Current = FindChildLeadingToTarget(InvalidTreeNode);
        if (!Current.IsValid())
        {
            return;
        }

        while (true)
        {
            // Copied, not referenced: ExpandNode creates nodes and can reallocate the node pool.
            const FFixedString CurrentPath = DirectoryListView.Get<FContentBrowserListViewItemData>(Current).Path;
            if (FStringView(CurrentPath.c_str(), CurrentPath.size()) == Target)
            {
                break;
            }

            DirectoryListView.ExpandNode(Current, DirectoryContext);

            const FTreeNodeID Next = FindChildLeadingToTarget(Current);
            if (!Next.IsValid())
            {
                break;
            }

            Current = Next;
        }

        DirectoryListView.SetSelectionSilent(Current);
        DirectoryListView.RequestScrollToNode(Current);
    }

    void FContentBrowserEditorTool::DrawContentBrowser(bool bIsFocused, ImVec2 Size)
    {
        constexpr float Padding = 1.0f;

        ImVec2 AdjustedSize = ImVec2(Size.x - 2 * Padding, 0.0f);

        ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(Padding, Padding));

        ImGui::BeginChild("Content", AdjustedSize, true, ImGuiWindowFlags_None);

        // Mouse thumb buttons. GLFW reports these as buttons 3 and 4, which ImGui forwards; hover rather
        // than focus, so they work without clicking into the browser first.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
        {
            if (ImGui::IsMouseClicked(3))
            {
                NavigateBack();
            }
            else if (ImGui::IsMouseClicked(4))
            {
                NavigateForward();
            }
        }

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
        
        // WantTextInput covers the inline rename and the search box alike: while either owns the
        // keyboard, Delete is editing text and must not reach the asset shortcut. The mouse is
        // usually over empty space while typing, so this fired on almost every rename.
        if (!ImGui::GetIO().WantTextInput
            && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()
            && ImGui::IsKeyPressed(ImGuiKey_Delete)
            && !ContentBrowserTileView.GetSelections().empty())
        {
            DeleteSelectedItems(ContentBrowserTileView.GetSelections());
        }
        
        ImGui::BeginHorizontal("Breadcrumbs");

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
        ImGui::BeginDisabled(!CanNavigateBack());
        if (ImGui::Button(LE_ICON_ARROW_LEFT "##NavBack"))
        {
            NavigateBack();
        }
        ImGui::EndDisabled();
        ImGuiX::TextTooltip("Back");

        ImGui::BeginDisabled(!CanNavigateForward());
        if (ImGui::Button(LE_ICON_ARROW_RIGHT "##NavForward"))
        {
            NavigateForward();
        }
        ImGui::EndDisabled();
        ImGuiX::TextTooltip("Forward");
        ImGui::PopStyleVar();

        ImGui::TextUnformatted("|");

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
                NavigateTo(CrumbPath);
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

    const FAssetData* FContentBrowserEditorTool::GetSelectedAsset() const
    {
        const TVector<FTileViewItem*>& Selections = ContentBrowserTileView.GetSelections();
        if (Selections.size() != 1)
        {
            return nullptr;   // ambiguous (or nothing) to hand to a single-object property
        }

        const FContentBrowserTileViewItem* Item = static_cast<const FContentBrowserTileViewItem*>(Selections[0]);
        if (Item == nullptr || !Item->IsAsset())
        {
            return nullptr;
        }

        const FFixedString Path(Item->GetVirtualPath().data(), Item->GetVirtualPath().size());
        return FAssetRegistry::Get().GetAssetByPath(Path);
    }

    void FContentBrowserEditorTool::DrawReimportAssetMenuItem(const FContentBrowserTileViewItem* ContentItem, bool bIsProtected)
    {
        const FFixedString AssetPath(ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().size());
        const FAssetData*  Data = FAssetRegistry::Get().GetAssetByPath(AssetPath);
        if (Data == nullptr)
        {
            return;
        }

        // Decided from the registry's class name, so drawing the menu never loads the asset.
        CClass* AssetClass = FindObject<CClass>(Data->AssetClass);
        CImporter* ImporterCDO = CImporterRegistry::Get().FindReimporter(AssetClass);
        if (ImporterCDO == nullptr)
        {
            return;
        }

        const FGuid AssetGUID = Data->AssetGUID;

        if (ImGui::MenuItem(LE_ICON_FILE_REPLACE " Reimport From File...", nullptr, false, !bIsProtected))
        {
            // One options window at a time; the modal manager allows a single owner, and a reimport
            // launched underneath a running batch import would lose its dialogue.
            if (bImportWindowOpen)
            {
                ImGuiX::Notifications::NotifyWarning("An import is already in progress; finish it first.");
                return;
            }

            CObject* Asset = LoadObject<CObject>(AssetGUID);
            if (Asset == nullptr)
            {
                ImGuiX::Notifications::NotifyError("Could not load '{0}' to reimport it.", AssetPath);
                return;
            }

            // Filter comes from the importer, so the dialog only offers files this asset type can actually
            // be built from -- the whole reason this is safer than "import over the top".
            TVector<FStringView> Extensions;
            ImporterCDO->GetSupportedExtensions(Extensions);

            FFixedString Patterns;
            for (const FStringView& Ext : Extensions)
            {
                if (!Patterns.empty())
                {
                    Patterns.append(";");
                }
                Patterns.append("*").append_convert(Ext.data(), Ext.length());
            }

            FFixedString Filter;
            Filter.append("Supported Files (").append(Patterns).append(")");
            Filter.push_back('\0');
            Filter.append(Patterns);
            Filter.push_back('\0');
            Filter.push_back('\0');

            const FString Previous = ImporterCDO->GetReimportSourcePath(Asset);

            FFixedString Picked;
            if (!Platform::OpenFileDialogue(Picked, "Reimport From File", Filter.c_str(),
                                            Previous.empty() ? nullptr : Previous.c_str()))
            {
                return;
            }

            // The dialog can hand back anything, so the extension is re-checked here rather than trusted;
            // the importers assume they were given a format they parse.
            if (!ImporterCDO->SupportsExtension(VFS::Extension(Picked)))
            {
                ImGuiX::Notifications::NotifyError("'{0}' is not a supported source file for this asset type.",
                                                   VFS::FileName(Picked).data());
                return;
            }

            StartReimport(AssetGUID, ImporterCDO->GetClass(), Picked);
        }
        ImGuiX::TextTooltip("{}", "Replace this asset's contents from a source file. The asset keeps its "
                                  "name, path and GUID, so everything referencing it stays pointed at it.");
    }

    void FContentBrowserEditorTool::StartReimport(const FGuid& AssetGUID, CClass* ImporterClass, const FFixedString& SourceFile)
    {
        CObject* Asset = LoadObject<CObject>(AssetGUID);
        if (Asset == nullptr)
        {
            return;
        }

        // The asset's own package path, so the options window's header reads as the thing being replaced
        // rather than as a destination that would be created.
        CPackage* Package = Asset->GetPackage();
        const FFixedString AssetPath = Package != nullptr ? Package->GetPackagePath() : FFixedString();

        bImportWindowOpen = true;

        CImporter* Importer = CImporterRegistry::CreateImporterOfClass(ImporterClass);
        const FImportRequest Request{ SourceFile, AssetPath };

        Task::AsyncTask(1, 1, [this, AssetGUID, Importer, Request](uint32, uint32, uint32)
        {
            const FStringView SourceName = VFS::FileName(Request.SourcePath, true);
            FFixedString Title(FFixedString::CtorSprintf(), "Reading %.*s", (int)SourceName.length(), SourceName.data());
            FScopedSlowTask SlowTask(1.0f, Title, "Parsing source file...");

            FString Error;
            const bool bParsed = Importer->ParseSource(Request, Error, &SlowTask);

            MainThread::Enqueue([this, AssetGUID, Importer, Request, bParsed, Error]() mutable
            {
                if (!bParsed)
                {
                    ImGuiX::Notifications::NotifyError("Failed to read \"{0}\": {1}", Request.SourcePath, Error);
                    CImporterRegistry::DestroyImporter(Importer);
                    bImportWindowOpen = false;
                    return;
                }

                // Importers with no options (texture, font, audio) go straight through, matching what a
                // fresh import of the same file does.
                if (!Importer->HasSettingsDialogue())
                {
                    FinishReimport(AssetGUID, Importer, Request.SourcePath);
                    bImportWindowOpen = false;
                    return;
                }

                Importer->PrepareSettingsPreview();

                ImportSettingsTable = MakeUnique<FPropertyTable>(Importer);
                ImportSettingsTable->SetShowSearchBar(false);

                struct FModalState
                {
                    bool bShouldClose = false;
                    bool bStarted     = false;
                };
                auto SharedState = MakeShared<FModalState>();

                ToolContext->PushModal("Reimport", {940, 900},
                    [this, AssetGUID, Importer, Request, SharedState]() mutable
                    {
                        // RemainingCount 0: a reimport is always a single file, so the window shows no
                        // "apply to all" affordance and the flag it returns is ignored.
                        bool bApplyToAll = false;

                        if (DrawImportWindow(Importer, Request, 0, SharedState->bShouldClose, bApplyToAll))
                        {
                            SharedState->bStarted = true;
                            FinishReimport(AssetGUID, Importer, Request.SourcePath);
                        }

                        if (SharedState->bShouldClose)
                        {
                            ImportSettingsTable.reset();
                            if (!SharedState->bStarted)
                            {
                                CImporterRegistry::DestroyImporter(Importer);
                            }
                            bImportWindowOpen = false;
                        }

                        return SharedState->bShouldClose;
                    });
            });
        });
    }

    void FContentBrowserEditorTool::FinishReimport(const FGuid& AssetGUID, CImporter* Importer,
                                                   const FFixedString& SourceFile)
    {
        // Stays on a worker like the import path does: the RHI takes multi-threaded submission, and the
        // geometry finalize is seconds of work that would otherwise freeze the editor.
        Task::AsyncTask(1, 1, [this, AssetGUID, Importer, SourceFile](uint32, uint32, uint32)
        {
            const FStringView SourceName = VFS::FileName(SourceFile, true);
            FFixedString Title(FFixedString::CtorSprintf(), "Reimporting %.*s", (int)SourceName.length(), SourceName.data());
            FScopedSlowTask SlowTask(1.0f, Title, "Processing...");

            CObject* Asset = LoadObject<CObject>(AssetGUID);
            if (Asset == nullptr)
            {
                MainThread::Enqueue([Importer, SourceFile]()
                {
                    CImporterRegistry::DestroyImporter(Importer);
                    ImGuiX::Notifications::NotifyError("The asset was gone before \"{0}\" could be applied.", SourceFile);
                });
                return;
            }

            if (!Importer->ReimportAsset(Asset, FImportRequest{ SourceFile, FFixedString() }, &SlowTask))
            {
                MainThread::Enqueue([Importer, SourceFile]()
                {
                    CImporterRegistry::DestroyImporter(Importer);
                    // ReimportAsset leaves the asset untouched when it fails, so there is nothing to undo.
                    ImGuiX::Notifications::NotifyError("Reimport failed; \"{0}\" was left unchanged.", SourceFile);
                });
                return;
            }

            // Thumbnail first, and on the main thread: it RENDERS the asset into the package's thumbnail
            // slot, so it has to happen before the save that embeds it -- and it would otherwise keep
            // showing the old contents, since nothing about the package path changed.
            MainThread::Enqueue([this, AssetGUID, Importer, SourceFile]()
            {
                CImporterRegistry::DestroyImporter(Importer);

                CObject* ReimportedAsset = LoadObject<CObject>(AssetGUID);
                if (ReimportedAsset == nullptr)
                {
                    ImGuiX::Notifications::NotifyError("The asset was gone before \"{0}\" could be applied.", SourceFile);
                    return;
                }

                CPackage* Package = ReimportedAsset->GetPackage();
                if (Package != nullptr)
                {
                    // No registered renderer (or a type with none) just leaves the old image; the cache
                    // drop below still forces the browser to re-read whatever the package now holds.
                    CThumbnailManager::Get().GenerateThumbnail(ReimportedAsset, Package);
                    CThumbnailManager::Get().InvalidateThumbnail(Package->GetName());
                }

                // Back off the main thread for the write: a mesh package is megabytes of meshlet data.
                Task::AsyncTask(1, 1, [this, AssetGUID, SourceFile](uint32, uint32, uint32)
                {
                    bool bSaved = false;
                    if (CObject* LoadedAsset = LoadObject<CObject>(AssetGUID))
                    {
                        if (CPackage* AssetPackage = LoadedAsset->GetPackage())
                        {
                            bSaved = CPackage::SavePackage(AssetPackage, AssetPackage->GetPackagePath());
                        }
                    }

                    MainThread::Enqueue([this, AssetGUID, SourceFile, bSaved]()
                    {
                        if (CObject* SavedAsset = LoadObject<CObject>(AssetGUID))
                        {
                            // The asset already existed, so this is a save notification and not a creation:
                            // the registry entry keeps its GUID and path, only the on-disk state moved.
                            FAssetRegistry::Get().AssetSaved(SavedAsset);
                            FCoreEditorDelegates::OnAssetSaved.Broadcast(SavedAsset);

                            // Nothing about the reference changed, so no open tool would otherwise notice
                            // that what it is showing is a different mesh/texture than the one it cached.
                            AssetEvents::BroadcastAssetDataChanged(SavedAsset);
                        }

                        RefreshContentBrowser();

                        if (bSaved)
                        {
                            ImGuiX::Notifications::NotifySuccess("Reimported from \"{0}\"", SourceFile);
                        }
                        else
                        {
                            ImGuiX::Notifications::NotifyWarning("Reimported from \"{0}\", but saving the package failed.", SourceFile);
                        }
                    });
                });
            });
        });
    }

    void FContentBrowserEditorTool::DrawDuplicateAssetMenuItem(const FContentBrowserTileViewItem* ContentItem, bool bIsProtected)
    {
        const FFixedString SourcePath(ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().size());
        const FAssetData*  Data = FAssetRegistry::Get().GetAssetByPath(SourcePath);
        if (Data == nullptr)
        {
            return;
        }

        if (ImGui::MenuItem(LE_ICON_CONTENT_DUPLICATE " Duplicate", nullptr, false, !bIsProtected))
        {
            CObject* Source = LoadObject<CObject>(Data->AssetGUID);
            if (Source == nullptr)
            {
                ImGuiX::Notifications::NotifyError("Could not load '{0}' to duplicate it.", SourcePath);
                return;
            }

            const FFixedString NewPath = MakeSiblingAssetPath(
                FStringView(SourcePath.c_str(), SourcePath.size()), "_Copy");

            // Package-level, not a flat property copy: a material keeps its node graph as a second
            // export found by name, and its nodes reference each other. DuplicateAssetPackage copies
            // every export and rewrites the references between them, so the copy is self-contained.
            CObject* Copy = DuplicateAssetPackage(Source, FStringView(NewPath.c_str(), NewPath.size()));
            if (Copy == nullptr)
            {
                ImGuiX::Notifications::NotifyError("Failed to duplicate '{0}'.", SourcePath);
                return;
            }

            FAssetRegistry::Get().AssetCreated(Copy);
            ImGuiX::Notifications::NotifySuccess("Duplicated to '{0}'.", NewPath);

            // A duplicate lands as "<Name>_Copy", which is even less likely to be the name you want
            // than a factory default.
            QueueRenameAfterCreate(FStringView(NewPath.c_str(), NewPath.size()));
        }
    }

    void FContentBrowserEditorTool::DrawCreateVariantMenuItem(const FContentBrowserTileViewItem* ContentItem)
    {
        const FFixedString SourcePath(ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().size());
        const FAssetData*  Data = FAssetRegistry::Get().GetAssetByPath(SourcePath);
        if (Data == nullptr)
        {
            return;
        }

        CClass* AssetClass = FindObject<CClass>(Data->AssetClass);
        if (AssetClass == nullptr || !AssetClass->IsChildOf(CPrefab::StaticClass()))
        {
            return;
        }

        if (!ImGui::MenuItem(LE_ICON_SOURCE_BRANCH " Create Variant"))
        {
            return;
        }

        CPrefab* Parent = LoadObject<CPrefab>(Data->AssetGUID);
        if (Parent == nullptr)
        {
            ImGuiX::Notifications::NotifyError("Could not load '{0}'.", SourcePath);
            return;
        }

        const FFixedString NewPath = MakeSiblingAssetPath(
            FStringView(SourcePath.c_str(), SourcePath.size()), "_Variant");

        // Not a package duplicate: a variant owns no data of its own, only a parent link. Everything it
        // shows comes from resolving that link, which is what keeps it following the parent.
        CPrefab* Variant = CFactory::CreateNewOf<CPrefab>(FStringView(NewPath.c_str(), NewPath.size()));
        if (Variant == nullptr)
        {
            ImGuiX::Notifications::NotifyError("Could not create '{0}'.", NewPath);
            return;
        }

        Variant->ParentPrefab = Parent;
        Variant->ResolveVariant();

        if (!CPackage::SavePackage(Variant->GetPackage(), FStringView(NewPath.c_str(), NewPath.size())))
        {
            ImGuiX::Notifications::NotifyError("Failed to save variant '{0}'.", NewPath);
            return;
        }

        FAssetRegistry::Get().AssetCreated(Variant);
        ImGuiX::Notifications::NotifySuccess("Created variant '{0}'.", NewPath);

        QueueRenameAfterCreate(FStringView(NewPath.c_str(), NewPath.size()));
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
            Platform::LaunchURL(UTF8_TO_TCHAR(Parent.c_str()));
        }

        // Class-registered actions ("Create Material Instance", ...). Plugins contribute here without
        // touching the content browser; see FAssetActionRegistry.
        if (bIsAsset)
        {
            const FFixedString AssetPath(ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().size());
            if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(AssetPath))
            {
                if (CClass* AssetClass = FindObject<CClass>(Data->AssetClass))
                {
                    TVector<const FAssetAction*> AvailableActions;
                    FAssetActionRegistry::Get().GatherActions(AssetClass, AvailableActions);

                    if (!AvailableActions.empty())
                    {
                        DrawMenuSection("ACTIONS");

                        const FAssetActionContext ActionContext{ ToolContext, Data };
                        for (const FAssetAction* Action : AvailableActions)
                        {
                            const bool bEnabled = Action->CanExecute == nullptr || Action->CanExecute(ActionContext);
                            if (ImGui::MenuItem(Action->Label.c_str(), nullptr, false, bEnabled))
                            {
                                Action->Execute(ActionContext);
                            }
                        }
                    }
                }
            }
        }

        DrawMenuSection("EDIT");

        bool bReplaceReferencesClicked = false;
        if (bIsAsset)
        {
            DrawDuplicateAssetMenuItem(ContentItem, bIsProtected);
            DrawCreateVariantMenuItem(ContentItem);
            DrawReimportAssetMenuItem(ContentItem, bIsProtected);

            bReplaceReferencesClicked = ImGui::MenuItem(LE_ICON_LINK_VARIANT " Replace References...");
            ImGuiX::TextTooltip("Point everything that references this asset at a different one, or null them.");
        }

        if (ImGui::MenuItem(LE_ICON_RENAME " Rename", "F2", false, !bIsProtected))
        {
            ContentBrowserTileView.BeginInlineRename(ContentItem);
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
        else if (bReplaceReferencesClicked)
        {
            OpenReplaceReferencesModal(ContentItem);
        }

        PopContextMenuItemStyle();
    }

    void FContentBrowserEditorTool::DrawMultiSelectionContextMenu(const TVector<FTileViewItem*>& Items)
    {
        int32 FolderCount    = 0;
        int32 AssetCount     = 0;
        int32 FileCount      = 0;
        int32 ProtectedCount = 0;

        for (FTileViewItem* Item : Items)
        {
            const auto* ContentItem = static_cast<const FContentBrowserTileViewItem*>(Item);

            if (ContentItem->IsProtected()) { ++ProtectedCount; }

            if (ContentItem->IsDirectory()) { ++FolderCount; }
            else if (ContentItem->IsAsset()) { ++AssetCount; }
            else { ++FileCount; }
        }

        FFixedString Title;
        Title.append_convert(eastl::to_string(Items.size()).c_str()).append(" items selected");

        FFixedString Subtitle;
        auto AppendPart = [&Subtitle](int32 Count, const char* Label)
        {
            if (Count == 0)
            {
                return;
            }
            if (!Subtitle.empty())
            {
                Subtitle.append(", ");
            }
            Subtitle.append_convert(eastl::to_string(Count).c_str()).append(" ").append(Label);
        };
        AppendPart(FolderCount, "folder(s)");
        AppendPart(AssetCount,  "asset(s)");
        AppendPart(FileCount,   "file(s)");

        PushContextMenuItemStyle();

        DrawMenuHeader(LE_ICON_SELECT_MULTIPLE, Title.c_str(), Subtitle.c_str(), kMenuAccent);

        DrawMenuSection("CLIPBOARD");

        if (ImGui::MenuItem(LE_ICON_CONTENT_COPY " Copy Paths"))
        {
            FFixedString Clipboard;
            for (FTileViewItem* Item : Items)
            {
                const auto* ContentItem = static_cast<const FContentBrowserTileViewItem*>(Item);
                if (!Clipboard.empty())
                {
                    Clipboard.append("\n");
                }
                Clipboard.append_convert(ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().size());
            }

            ImGui::SetClipboardText(Clipboard.c_str());
            ImGuiX::Notifications::NotifyInfo("{0} paths copied to clipboard", Items.size());
        }

        ImGui::Spacing();
        ImGui::Separator();

        const bool bWorldActive  = IsAnyWorldPlayingOrSimulating();
        const bool bAllProtected = ProtectedCount == (int32)Items.size();

        ImGui::PushStyleColor(ImGuiCol_Text,          kMenuDanger);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kMenuDangerHover);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.85f, 0.22f, 0.24f, 0.85f));
        const bool bDeleteClicked = ImGui::MenuItem(LE_ICON_TRASH_CAN " Delete Selected", "Del", false, !bAllProtected && !bWorldActive);
        ImGui::PopStyleColor(3);

        // Protected entries do not block the batch, they are simply skipped, so say so up front rather
        // than only in the notification after the fact.
        if (bAllProtected || bWorldActive || ProtectedCount > 0)
        {
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Tiny);
            ImGui::PushStyleColor(ImGuiCol_Text, kMenuTextDim);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);

            if (bWorldActive)
            {
                ImGui::TextUnformatted(LE_ICON_LOCK " Stop play to delete");
            }
            else if (bAllProtected)
            {
                ImGui::TextUnformatted(LE_ICON_LOCK " Every selected entry is protected");
            }
            else
            {
                ImGui::Text(LE_ICON_LOCK " %d protected entr%s will be skipped", ProtectedCount, ProtectedCount == 1 ? "y" : "ies");
            }

            ImGui::PopStyleColor();
            ImGuiX::Font::PopFont();
        }

        PopContextMenuItemStyle();

        // Last, after every read of Items: DeleteSelectedItems clears the selection and queues the tiles
        // for destruction.
        if (bDeleteClicked)
        {
            DeleteSelectedItems(Items);
        }
    }

    void FContentBrowserEditorTool::DeleteSelectedItems(const TVector<FTileViewItem*>& Items)
    {
        if (Items.empty())
        {
            return;
        }

        // Snapshotted before anything can rebuild the tile tree, which frees the items themselves.
        TVector<FFixedString> Deletable;
        Deletable.reserve(Items.size());

        int32 ProtectedCount = 0;
        for (FTileViewItem* Item : Items)
        {
            const auto* ContentItem = static_cast<const FContentBrowserTileViewItem*>(Item);
            if (ContentItem->IsProtected())
            {
                ++ProtectedCount;
                continue;
            }

            Deletable.emplace_back(ContentItem->GetVirtualPath().data(), ContentItem->GetVirtualPath().size());
        }

        RequestDeletion(Move(Deletable), ProtectedCount);
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
            // Same reasoning as a new asset: "NewFolder" is a placeholder, not a name.
            QueueRenameAfterCreate(FStringView(FinalPath.c_str(), FinalPath.size()));
        }

        // Aggregated asset creation submenu (factory-driven), grouped into per-category submenus.
        const TVector<CFactory*>& Factories = CFactoryRegistry::Get().GetFactories();
        if (!bScriptDir && ImGui::BeginMenu(LE_ICON_PLUS_BOX " New Asset"))
        {
            auto CreateFromFactory = [this](CFactory* Factory)
            {
                if (!IsAssetCreationAllowed(FStringView(SelectedPath.c_str(), SelectedPath.size())))
                {
                    ImGuiX::Notifications::NotifyWarning(
                        "Cannot create assets in \"{0}\". Assets have to live under a Content folder.",
                        SelectedPath);
                    return;
                }

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

                            // The dialogue hands the actual creation to a task, so the tile may not
                            // exist for several frames. The request just waits for it.
                            QueueRenameAfterCreate(FStringView(Path.c_str(), Path.size()));
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
                        QueueRenameAfterCreate(FStringView(Path.c_str(), Path.size()));
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
                if (Factory->GetAssetClass() == nullptr)
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

            // A skeleton rather than an empty file. An empty .rml opens to a blank preview with no
            // indication of what is missing, and the one rule nothing else teaches -- RmlUi has no
            // user-agent stylesheet, so every element is display:inline until a rule says otherwise --
            // costs a new author an afternoon when their first panel silently collapses.
            VFS::WriteFile(NewWidgetPath,
                "<rml>\n"
                "<head>\n"
                "    <title>New Widget</title>\n"
                "    <style>\n"
                "        /* RmlUi has no HTML-like default stylesheet: EVERY element starts as\n"
                "           display:inline, so containers must be made block (or flex) or they\n"
                "           collapse and ignore width/padding/margin. */\n"
                "        div { display: block; box-sizing: border-box; }\n"
                "\n"
                "        /* Fill the view and center the panel. 'dp' scales with the display. */\n"
                "        body { width: 100%; height: 100%; display: flex;\n"
                "               align-items: center; justify-content: center;\n"
                "               color: #cdd6f4; font-size: 16dp; }\n"
                "\n"
                "        .panel { padding: 24dp; background-color: #1e1e2e;\n"
                "                 border-width: 1dp; border-color: #45475a; border-radius: 8dp; }\n"
                "    </style>\n"
                "</head>\n"
                "<body>\n"
                "    <div class=\"panel\">\n"
                "        <div>Hello from RmlUi.</div>\n"
                "    </div>\n"
                "</body>\n"
                "</rml>\n");
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
                TVector<FFixedString> SelectedFiles;
                const char* Filter = "Supported Assets (*.wav;*.png;*.jpg;*.hdr;*.fbx;*.gltf;*.glb;*.obj;*.ttf;*.otf)\0*.wav;*.png;*.jpg;*.hdr;*.fbx;*.gltf;*.glb;*.obj;*.ttf;*.otf\0All Files (*.*)\0*.*\0";
                if (Platform::OpenFileDialogueMulti(SelectedFiles, "Import Assets", Filter))
                {
                    TryImport(SelectedFiles);
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
            Platform::LaunchURL(UTF8_TO_TCHAR(Target));
        }
    }
}
