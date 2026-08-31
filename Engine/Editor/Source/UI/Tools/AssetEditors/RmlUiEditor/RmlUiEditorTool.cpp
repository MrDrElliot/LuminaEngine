#include "RmlUiEditorTool.h"

#include <string>
#include <string_view>
#include <vector>
#include "Config/Config.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Object/ObjectCore.h"
#include "Settings/EditorSettings.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHITexture.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiKeyCapture.h"
#include "Tools/UI/ImGui/ImGuiRenderer.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "UI/RmlUiBridge.h"
#include "UI/RmlUiRenderer.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <random>
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace
    {
        const char* RmlEditorWindowName    = "RmlEditor";
        const char* RmlPreviewWindowName   = "RmlPreview";
        const char* RmlHierarchyWindowName = "RmlHierarchy";
        const char* RmlInspectorWindowName = "RmlInspector";

        // Section headings were an ImVec4 literal repeated at every call site.
        constexpr ImVec4 kSectionHeader{0.60f, 0.85f, 1.00f, 1.00f};

        void SectionHeader(const char* Icon, const char* Label)
        {
            ImGui::TextColored(kSectionHeader, "%s %s", Icon, Label);
        }

        FString DisplayNameFromPath(FStringView Path)
        {
            const FStringView Name = VFS::FileName(Path);
            return FString(Name.data(), Name.size());
        }

        // A random hue at high saturation and value, so the result stays vibrant rather than muddy.
        FVector3 RandomVibrantColor()
        {
            static std::mt19937 Rng{std::random_device{}()};
            std::uniform_real_distribution<float> Hue(0.0f, 1.0f);
            std::uniform_real_distribution<float> Sat(0.55f, 0.9f);
            std::uniform_real_distribution<float> Val(0.75f, 1.0f);
            float R, G, B;
            ImGui::ColorConvertHSVtoRGB(Hue(Rng), Sat(Rng), Val(Rng), R, G, B);
            return FVector3(R, G, B);
        }

        // Standard 16 by 9 game-UI resolutions plus 4K, with index 0 meaning fit to pane.
        struct FResolutionPreset
        {
            const char* Label;
            uint32      Width;
            uint32      Height;
        };
        const FResolutionPreset ResolutionPresets[] =
        {
            { "Fit to pane",   0u,    0u    },
            { "1280x720",      1280u, 720u  },
            { "1920x1080",     1920u, 1080u },
            { "2560x1440",     2560u, 1440u },
            { "3840x2160",     3840u, 2160u },
            { "Custom",        0u,    0u    },
        };
        constexpr int CustomPresetIndex = (int)(sizeof(ResolutionPresets) / sizeof(ResolutionPresets[0])) - 1;


        struct FRmlSnippet
        {
            const char* Label;
            const char* Body;
        };

        const FRmlSnippet kRmlDocSnippets[] =
        {
            { "Document skeleton",
                "<rml>\n"
                "<head>\n"
                "\t<title>Untitled</title>\n"
                "\t<style>\n"
                "\t\tbody { font-family: \"Source Sans Pro\"; color: #fff; }\n"
                "\t</style>\n"
                "</head>\n"
                "<body>\n"
                "\t\n"
                "</body>\n"
                "</rml>\n" },
            { "Inline <style> block",   "<style>\n\t\n</style>\n" },
            { "<link rel=\"stylesheet\">", "<link rel=\"stylesheet\" type=\"text/rcss\" href=\"\"/>\n" },
            { "<div id=...>",           "<div id=\"\" class=\"\">\n\t\n</div>\n" },
            { "<button>",               "<button onclick=\"\">Label</button>\n" },
            { "<input text>",           "<input type=\"text\" name=\"\" value=\"\"/>\n" },
            { "<input checkbox>",       "<input type=\"checkbox\" name=\"\" checked/>\n" },
            { "<select / option>",      "<select name=\"\">\n\t<option value=\"a\">A</option>\n\t<option value=\"b\">B</option>\n</select>\n" },
            { "<tabset / panel>",       "<tabset>\n\t<tab>One</tab>\n\t<tab>Two</tab>\n\t<panels>\n\t\t<panel>\n\t\t\t\n\t\t</panel>\n\t\t<panel>\n\t\t\t\n\t\t</panel>\n\t</panels>\n</tabset>\n" },
            { "<progress>",             "<progress value=\"0.5\" max-value=\"1.0\"/>\n" },
            { "<handle> (drag)",        "<handle move-target=\"#parent\">drag</handle>\n" },
            { "<template> include",     "<template name=\"\" content=\"\" src=\"\"/>\n" },
        };

        const FRmlSnippet kRcssSnippets[] =
        {
            { "selector { } block",        "selector {\n\t\n}\n" },
            { "id selector",                "#id {\n\t\n}\n" },
            { "class selector",             ".class {\n\t\n}\n" },
            { "Pseudo :hover",              "selector:hover {\n\t\n}\n" },
            { "Pseudo :active",             "selector:active {\n\t\n}\n" },
            { "Pseudo :checked",            "selector:checked {\n\t\n}\n" },
            { "Pseudo :focus",              "selector:focus {\n\t\n}\n" },
            { "Centered flex container",    "display: flex;\njustify-content: center;\nalign-items: center;\n" },
            { "Vertical flex column",       "display: flex;\nflex-direction: column;\ngap: 8dp;\n" },
            { "Absolute fill",              "position: absolute;\nleft: 0; top: 0; right: 0; bottom: 0;\n" },
            { "Padding/margin shorthand",   "padding: 8dp 16dp;\nmargin: 4dp 0;\n" },
            { "transition",                 "transition: background-color 0.15s linear, color 0.15s linear;\n" },
            { "Decorator (gradient)",       "decorator: gradient( vertical #1f2a36 #0e1620 );\n" },
            { "Border + radius",            "border: 1dp #444;\nborder-radius: 4dp;\n" },
            { "Drop shadow font-effect",    "font-effect: shadow(0dp 1dp #000a);\n" },
            { "Outline font-effect",        "font-effect: outline(1dp #000);\n" },
            { "Glow font-effect",           "font-effect: glow(1dp 0dp 0dp #4af);\n" },
        };

        ImU32 ToU32(const ImVec4& C) { return ImGui::ColorConvertFloat4ToU32(C); }

        // Hyphens are allowed after the start, so border-top-left-radius highlights as one token.
        TextEditor::Iterator GetRmlIdentifier(TextEditor::Iterator start, TextEditor::Iterator end)
        {
            if (start < end && TextEditor::CodePoint::isXidStart(*start))
            {
                start++;
                while (start < end && (TextEditor::CodePoint::isXidContinue(*start) || *start == '-'))
                {
                    start++;
                }
            }
            return start;
        }

        // SourceUrl stays the .rcss path, so relative references resolve from its own folder.
        std::string BuildStylesheetSpecimen(const std::string& Rcss)
        {
            std::string Doc;
            Doc.reserve(Rcss.size() + 2400);
            Doc += "<rml><head>\n<style>\n";
            Doc += Rcss;
            Doc +=
                "\n</style>\n<style>\n"
                // The context root already carries the default family, and naming an unregistered one logs per frame.
                "body { padding: 22dp; }\n"
                ".spec-label { display:block; font-size:11dp; color:#6c7086; text-transform:uppercase; letter-spacing:1dp; margin-top:16dp; margin-bottom:6dp; }\n"
                ".spec-row { display:flex; flex-direction:row; align-items:center; }\n"
                ".spec-row > * { margin-right:8dp; }\n"
                "</style>\n</head>\n<body>\n"
                "<div class=\"h1\">Heading One</div>\n"
                "<div class=\"h2\">Heading Two</div>\n"
                "<p>The quick brown fox jumps. "
                "<span class=\"text-primary\">primary</span> "
                "<span class=\"text-success\">success</span> "
                "<span class=\"text-warning\">warning</span> "
                "<span class=\"text-danger\">danger</span> "
                "<span class=\"text-muted\">muted</span></p>\n"
                "<div class=\"spec-label\">Buttons</div>\n"
                "<div class=\"spec-row\">"
                "<button class=\"btn\">Default</button>"
                "<button class=\"btn btn-primary\">Primary</button>"
                "<button class=\"btn btn-danger\">Danger</button>"
                "<button class=\"btn btn-ghost\">Ghost</button></div>\n"
                "<div class=\"spec-label\">Badges</div>\n"
                "<div class=\"spec-row\">"
                "<span class=\"badge\">default</span>"
                "<span class=\"badge badge-primary\">primary</span>"
                "<span class=\"badge badge-success\">success</span>"
                "<span class=\"badge badge-warning\">warning</span>"
                "<span class=\"badge badge-danger\">danger</span></div>\n"
                "<div class=\"spec-label\">Panel + bars</div>\n"
                "<div class=\"panel\">"
                "<div class=\"hud-title\">Panel Title</div>"
                "<div class=\"bar hp\" style=\"margin-top:10dp;\"><div class=\"fill\" style=\"width:72%;\"/></div>"
                "<div class=\"bar\" style=\"margin-top:8dp;\"><div class=\"fill\" style=\"width:48%;\"/></div>"
                "<div class=\"bar mana\" style=\"margin-top:8dp;\"><div class=\"fill\" style=\"width:90%;\"/></div></div>\n"
                "<div class=\"spec-label\">Keys</div>\n"
                "<div class=\"spec-row\"><span class=\"kbd\">Ctrl</span><span class=\"kbd\">S</span></div>\n"
                "</body></rml>\n";
            return Doc;
        }

        // A template root is reusable chrome, and LoadDocumentFromMemory trips the injection handler.
        bool IsTemplateDocument(const std::string& Body)
        {
            size_t i = 0;
            while (i < Body.size() && static_cast<unsigned char>(Body[i]) <= ' ')
            {
                ++i;
            }
            static const char* Tag = "<template";
            const size_t N = std::strlen(Tag);
            return (Body.size() - i >= N) && (std::memcmp(Body.data() + i, Tag, N) == 0);
        }

        // Swaps the template wrapper for rml so the framed body previews with an empty content slot.
        std::string BuildTemplatePreview(const std::string& Body)
        {
            std::string Doc = Body;
            const size_t Open = Doc.find("<template");
            if (Open != std::string::npos)
            {
                const size_t Close = Doc.find('>', Open);
                if (Close != std::string::npos)
                {
                    Doc.replace(Open, Close - Open + 1, "<rml>");
                }
            }
            const size_t End = Doc.find("</template>");
            if (End != std::string::npos)
            {
                Doc.replace(End, std::strlen("</template>"), "</rml>");
            }
            return Doc;
        }

        // Composition designer buffer parsing and edit helpers, operating on the raw .rml text.

        // Quote-aware, so an attribute value containing a closing angle bracket does not fool it.
        struct FSlotTagLoc
        {
            bool        bFound = false;
            size_t      TagStart = 0;        // index of '<'
            size_t      TagEnd = 0;          // index of the open tag's closing '>'
            bool        bSelfClosing = false;
            std::string TagName;
        };

        FSlotTagLoc LocateSlotTag(const std::string& Text, const std::string& Id)
        {
            FSlotTagLoc Loc;
            if (Id.empty())
            {
                return Loc;
            }

            const std::string Needles[2] = { "id=\"" + Id + "\"", "id='" + Id + "'" };
            size_t IdPos = std::string::npos;
            for (const std::string& N : Needles)
            {
                const size_t P = Text.find(N);
                if (P != std::string::npos && (IdPos == std::string::npos || P < IdPos))
                {
                    IdPos = P;
                }
            }
            if (IdPos == std::string::npos)
            {
                return Loc;
            }

            const size_t Lt = Text.rfind('<', IdPos);
            if (Lt == std::string::npos)
            {
                return Loc;
            }

            bool InSingle = false, InDouble = false;
            size_t Gt = std::string::npos;
            for (size_t i = Lt + 1; i < Text.size(); ++i)
            {
                const char C = Text[i];
                if (InSingle) { if (C == '\'') InSingle = false; continue; }
                if (InDouble) { if (C == '"')  InDouble = false; continue; }
                if (C == '\'') { InSingle = true; continue; }
                if (C == '"')  { InDouble = true; continue; }
                if (C == '>')  { Gt = i; break; }
            }
            if (Gt == std::string::npos)
            {
                return Loc;
            }

            Loc.bFound = true;
            Loc.TagStart = Lt;
            Loc.TagEnd = Gt;
            Loc.bSelfClosing = (Gt > Lt + 1) && (Text[Gt - 1] == '/');

            size_t N = Lt + 1;
            while (N < Gt && !std::isspace((unsigned char)Text[N]) && Text[N] != '/' && Text[N] != '>')
            {
                ++N;
            }
            Loc.TagName = Text.substr(Lt + 1, N - (Lt + 1));
            return Loc;
        }

        // Reads a slotted template's src back out of the buffer, since the directive leaves no DOM element.
        std::string ParseSlotAssignment(const std::string& Text, const std::string& Id)
        {
            const FSlotTagLoc Loc = LocateSlotTag(Text, Id);
            if (!Loc.bFound || Loc.bSelfClosing)
            {
                return {};
            }
            size_t i = Loc.TagEnd + 1;
            while (i < Text.size() && (unsigned char)Text[i] <= ' ')
            {
                ++i;
            }
            static const char* Tpl = "<template";
            const size_t TplLen = std::strlen(Tpl);
            if (i + TplLen > Text.size() || Text.compare(i, TplLen, Tpl) != 0)
            {
                return {};
            }
            const size_t End = Text.find('>', i);
            if (End == std::string::npos)
            {
                return {};
            }
            const std::string Tag = Text.substr(i, End - i);
            const size_t Sp = Tag.find("src=");
            if (Sp == std::string::npos || Sp + 4 >= Tag.size())
            {
                return {};
            }
            const char Quote = Tag[Sp + 4];
            if (Quote != '"' && Quote != '\'')
            {
                return {};
            }
            const size_t ValStart = Sp + 5;
            const size_t ValEnd = Tag.find(Quote, ValStart);
            if (ValEnd == std::string::npos)
            {
                return {};
            }
            return Tag.substr(ValStart, ValEnd - ValStart);
        }

        void OffsetToLineCol(const std::string& Text, size_t Offset, int TabSize, int& OutLine, int& OutCol)
        {
            if (TabSize < 1) TabSize = 4;
            int Line = 0;
            size_t LineStart = 0;
            const size_t Clamp = Math::Min(Offset, Text.size());
            for (size_t i = 0; i < Clamp; ++i)
            {
                if (Text[i] == '\n') { ++Line; LineStart = i + 1; }
            }
            int Col = 0;
            for (size_t i = LineStart; i < Clamp; ++i)
            {
                if (Text[i] == '\t') Col += TabSize - (Col % TabSize);
                else ++Col;
            }
            OutLine = Line;
            OutCol = Col;
        }

        bool ContainsCI(const FString& Haystack, const char* Needle)
        {
            if (Needle == nullptr || *Needle == '\0')
            {
                return true;
            }
            std::string H(Haystack.c_str(), Haystack.size());
            std::string N(Needle);
            Algo::Transform(H, H.begin(), [](unsigned char C){ return (char)std::tolower(C); });
            Algo::Transform(N, N.begin(), [](unsigned char C){ return (char)std::tolower(C); });
            return H.find(N) != std::string::npos;
        }

        std::string TrimStr(const std::string& S)
        {
            size_t A = 0, B = S.size();
            while (A < B && (unsigned char)S[A] <= ' ') ++A;
            while (B > A && (unsigned char)S[B - 1] <= ' ') --B;
            return S.substr(A, B - A);
        }

        // Splits an inline style value into ordered key and value pairs.
        std::vector<std::pair<std::string, std::string>> ParseStyle(const std::string& Style)
        {
            std::vector<std::pair<std::string, std::string>> Out;
            size_t i = 0;
            while (i < Style.size())
            {
                const size_t Semi = Style.find(';', i);
                const std::string Decl = Style.substr(i, (Semi == std::string::npos ? Style.size() : Semi) - i);
                const size_t Colon = Decl.find(':');
                if (Colon != std::string::npos)
                {
                    const std::string Key = TrimStr(Decl.substr(0, Colon));
                    const std::string Val = TrimStr(Decl.substr(Colon + 1));
                    if (!Key.empty())
                    {
                        Out.push_back({ Key, Val });
                    }
                }
                if (Semi == std::string::npos) break;
                i = Semi + 1;
            }
            return Out;
        }

        // Value of one inline style property on a slot element ("" if absent).
        std::string GetInlineStyleProp(const std::string& Text, const std::string& Id, const char* Prop)
        {
            const FSlotTagLoc Loc = LocateSlotTag(Text, Id);
            if (!Loc.bFound)
            {
                return {};
            }
            for (const char* Key : { "style=\"", "style='" })
            {
                const size_t P = Text.find(Key, Loc.TagStart);
                if (P != std::string::npos && P < Loc.TagEnd)
                {
                    const char Quote = Key[6];
                    const size_t VS = P + 7;
                    const size_t VE = Text.find(Quote, VS);
                    if (VE != std::string::npos && VE <= Loc.TagEnd)
                    {
                        for (const auto& KV : ParseStyle(Text.substr(VS, VE - VS)))
                        {
                            if (KV.first == Prop) return KV.second;
                        }
                    }
                    return {};
                }
            }
            return {};
        }

        // sscanf reads each component's leading number and ignores the unit, and we only author dp.
        ImVec2 ParseTranslateDp(const std::string& Transform)
        {
            const size_t Open = Transform.find('(');
            if (Open == std::string::npos)
            {
                return ImVec2(0.0f, 0.0f);
            }
            const size_t Close = Transform.find(')', Open);
            if (Close == std::string::npos)
            {
                return ImVec2(0.0f, 0.0f);
            }
            const std::string Inner = Transform.substr(Open + 1, Close - Open - 1);
            float X = 0.0f, Y = 0.0f;
            std::sscanf(Inner.c_str(), "%f", &X);
            const size_t Comma = Inner.find(',');
            if (Comma != std::string::npos)
            {
                std::sscanf(Inner.c_str() + Comma + 1, "%f", &Y);
            }
            return ImVec2(X, Y);
        }

        // Depth-aware over the same tag name, so a nested same-tag open raises depth.
        struct FCloseTagLoc { size_t Start = std::string::npos; size_t End = std::string::npos; };
        FCloseTagLoc FindMatchingClose(const std::string& Text, const FSlotTagLoc& Open)
        {
            FCloseTagLoc Out;
            if (Open.bSelfClosing)
            {
                return Out;
            }
            const std::string& Tag = Open.TagName;
            int Depth = 1;
            size_t Pos = Open.TagEnd + 1;
            auto Boundary = [&](size_t After) { return After >= Text.size() || Text[After] == '>' || Text[After] == '/' || std::isspace((unsigned char)Text[After]); };

            while (Pos < Text.size())
            {
                const size_t Lt = Text.find('<', Pos);
                if (Lt == std::string::npos) break;

                if (Lt + 1 < Text.size() && Text[Lt + 1] == '/')
                {
                    const size_t N = Lt + 2;
                    if (Text.compare(N, Tag.size(), Tag) == 0 && (N + Tag.size() >= Text.size() || Text[N + Tag.size()] == '>'))
                    {
                        if (--Depth == 0)
                        {
                            const size_t Gt = Text.find('>', Lt);
                            Out.Start = Lt;
                            Out.End = (Gt == std::string::npos) ? Text.size() : Gt + 1;
                            return Out;
                        }
                    }
                    Pos = Lt + 2;
                    continue;
                }

                const size_t N = Lt + 1;
                if (Text.compare(N, Tag.size(), Tag) == 0 && Boundary(N + Tag.size()))
                {
                    bool InS = false, InD = false;
                    size_t Gt = std::string::npos;
                    for (size_t i = N + Tag.size(); i < Text.size(); ++i)
                    {
                        const char C = Text[i];
                        if (InS) { if (C == '\'') InS = false; continue; }
                        if (InD) { if (C == '"')  InD = false; continue; }
                        if (C == '\'') { InS = true; continue; }
                        if (C == '"')  { InD = true; continue; }
                        if (C == '>')  { Gt = i; break; }
                    }
                    if (Gt != std::string::npos)
                    {
                        if (!((Gt > N) && Text[Gt - 1] == '/')) ++Depth;  // not self-closing -> nests
                        Pos = Gt + 1;
                        continue;
                    }
                }
                Pos = Lt + 1;
            }
            return Out;
        }

        
        // Position-keyed rather than id-keyed, for walking siblings that may carry no id.
        FSlotTagLoc ParseElementAt(const std::string& Text, size_t Lt)
        {
            FSlotTagLoc Loc;
            if (Lt >= Text.size() || Text[Lt] != '<')
            {
                return Loc;
            }
            bool InS = false, InD = false;
            size_t Gt = std::string::npos;
            for (size_t i = Lt + 1; i < Text.size(); ++i)
            {
                const char C = Text[i];
                if (InS) { if (C == '\'') InS = false; continue; }
                if (InD) { if (C == '"')  InD = false; continue; }
                if (C == '\'') { InS = true; continue; }
                if (C == '"')  { InD = true; continue; }
                if (C == '>')  { Gt = i; break; }
            }
            if (Gt == std::string::npos)
            {
                return Loc;
            }
            Loc.bFound = true;
            Loc.TagStart = Lt;
            Loc.TagEnd = Gt;
            Loc.bSelfClosing = (Gt > Lt + 1) && Text[Gt - 1] == '/';
            size_t N = Lt + 1;
            while (N < Gt && !std::isspace((unsigned char)Text[N]) && Text[N] != '/' && Text[N] != '>')
            {
                ++N;
            }
            Loc.TagName = Text.substr(Lt + 1, N - (Lt + 1));
            return Loc;
        }

        // Not self-closing, no child elements, and either a text-ish tag or already holding text.
        bool GetEditableInnerText(const std::string& Text, const std::string& Id, std::string& OutInner)
        {
            const FSlotTagLoc Open = LocateSlotTag(Text, Id);
            if (!Open.bFound || Open.bSelfClosing)
            {
                return false;
            }
            const FCloseTagLoc Close = FindMatchingClose(Text, Open);
            if (Close.Start == std::string::npos)
            {
                return false;
            }
            const std::string Inner = Text.substr(Open.TagEnd + 1, Close.Start - (Open.TagEnd + 1));
            if (Inner.find('<') != std::string::npos)
            {
                return false; // has child elements
            }
            static const char* const TextTags[] = { "span","p","button","label","a","h1","h2","h3","h4","h5","h6","li","td","th","strong","em","b","i" };
            bool bTextTag = false;
            for (const char* T : TextTags) { if (Open.TagName == T) { bTextTag = true; break; } }
            if (!bTextTag && TrimStr(Inner).empty())
            {
                return false; // a plain empty container -> don't clutter the inspector with a text field
            }
            OutInner = TrimStr(Inner);
            return true;
        }

        // The id attribute on an element's open tag, or "" if none.
        std::string ParseIdAttr(const std::string& Text, const FSlotTagLoc& Tag)
        {
            for (const char* Key : { "id=\"", "id='" })
            {
                const size_t P = Text.find(Key, Tag.TagStart);
                if (P != std::string::npos && P < Tag.TagEnd)
                {
                    const char Q = Key[3];
                    const size_t VS = P + 4;
                    const size_t VE = Text.find(Q, VS);
                    if (VE != std::string::npos && VE <= Tag.TagEnd)
                    {
                        return Text.substr(VS, VE - VS);
                    }
                }
            }
            return {};
        }

        // Parsed from the SOURCE, so it shows exactly what the user can edit and hides widget internals.
        struct FRmlHierarchyItem
        {
            std::string Tag;
            std::string Id;      // empty when the element has no id, which keeps it out of the live DOM
            size_t      OpenLt = 0;
            bool        bIsBody = false;
        };


        struct FSourceNode
        {
            std::string Tag;
            std::string Id;        // "" if the element has no id yet
            size_t      OpenLt;    // byte offset of '<' in the source
            int         Depth;     // nesting depth under <body>
        };

        // Depth is an open and close counter, which assumes the well-formed nesting the preview validates.
        void ParseSourceElements(const std::string& Text, std::vector<FSourceNode>& Out)
        {
            Out.clear();
            const size_t BodyOpen = Text.find("<body");
            if (BodyOpen == std::string::npos) return;
            size_t Pos = Text.find('>', BodyOpen);
            if (Pos == std::string::npos) return;
            ++Pos;

            int Depth = 0;
            while (Pos < Text.size())
            {
                const size_t Lt = Text.find('<', Pos);
                if (Lt == std::string::npos) break;

                if (Text.compare(Lt, 4, "<!--") == 0)
                {
                    const size_t E = Text.find("-->", Lt);
                    Pos = (E == std::string::npos) ? Text.size() : E + 3;
                    continue;
                }
                if (Lt + 1 < Text.size() && (Text[Lt + 1] == '!' || Text[Lt + 1] == '?'))
                {
                    const size_t E = Text.find('>', Lt);
                    Pos = (E == std::string::npos) ? Text.size() : E + 1;
                    continue;
                }
                if (Lt + 1 < Text.size() && Text[Lt + 1] == '/')
                {
                    const size_t Gt = Text.find('>', Lt);
                    if (Gt == std::string::npos) break;
                    size_t N = Lt + 2, E = N;
                    while (E < Gt && !std::isspace((unsigned char)Text[E]) && Text[E] != '>') ++E;
                    if (Text.compare(N, E - N, "body") == 0) break;
                    if (Depth > 0) --Depth;
                    Pos = Gt + 1;
                    continue;
                }

                const FSlotTagLoc T = ParseElementAt(Text, Lt);
                if (!T.bFound)
                {
                    Pos = Lt + 1;
                    continue;
                }
                FSourceNode Node;
                Node.Tag = T.TagName;
                Node.Id = ParseIdAttr(Text, T);
                Node.OpenLt = Lt;
                Node.Depth = Depth;
                Out.push_back(std::move(Node));
                if (!T.bSelfClosing) ++Depth;
                Pos = T.TagEnd + 1;
            }
        }

        const TextEditor::Language* GetRmlLanguage(bool bStylesheet)
        {
            // The TextEditor supports only ONE multi-line pair, and .rml and .rcss want different ones.
            static bool InitializedDoc = false;
            static bool InitializedCss = false;
            static TextEditor::Language LangDoc;
            static TextEditor::Language LangCss;

            TextEditor::Language& Lang = bStylesheet ? LangCss : LangDoc;
            bool& Initialized = bStylesheet ? InitializedCss : InitializedDoc;
            if (Initialized)
            {
                return &Lang;
            }

            Lang.name = "RML/RCSS";
            Lang.caseSensitive = false;
            if (bStylesheet)
            {
                Lang.commentStart = "/*";
                Lang.commentEnd = "*/";
            }
            else
            {
                // The built-in tracker carries in-comment state across lines, so the tokenizer no longer does.
                Lang.commentStart = "<!--";
                Lang.commentEnd = "-->";
            }
            Lang.hasSingleQuotedStrings = true;
            Lang.hasDoubleQuotedStrings = true;
            Lang.stringEscape = '\\';
            Lang.getIdentifier = GetRmlIdentifier;

            // The hex-color token anchors the swatch overlay and stops it lexing as an identifier.
            Lang.customTokenizer = [](TextEditor::Iterator start, TextEditor::Iterator end, TextEditor::Color& color) -> TextEditor::Iterator
            {
                // A hash followed by 3, 4, 6 or 8 hex digits, the CSS color literal.
                if (start != end && *start == '#')
                {
                    auto cursor = start;
                    ++cursor;
                    int digits = 0;
                    while (cursor != end && digits < 8)
                    {
                        const auto c = *cursor;
                        const bool isHex =
                            (c >= '0' && c <= '9') ||
                            (c >= 'a' && c <= 'f') ||
                            (c >= 'A' && c <= 'F');
                        if (!isHex)
                        {
                            break;
                        }
                        ++cursor;
                        ++digits;
                    }
                    if (digits == 3 || digits == 4 || digits == 6 || digits == 8)
                    {
                        color = TextEditor::Color::number;
                        return cursor;
                    }
                }

                return start;
            };

            // RML elements (HTML subset + RmlUI-specific widgets).
            static const char* const Tags[] = {
                "rml", "head", "body", "title", "link", "style", "script", "meta",
                "div", "span", "p", "br", "hr",
                "h1", "h2", "h3", "h4", "h5", "h6",
                "b", "i", "u", "em", "strong", "small", "sub", "sup",
                "a", "img", "icon",
                "ul", "ol", "li",
                "table", "tr", "td", "th", "thead", "tbody", "tfoot",
                "form", "input", "button", "select", "option", "textarea", "label",
                "tabset", "tab", "panels", "panel", "handle", "progress",
                "dataselect", "datagrid", "datagridrow", "datagridcell", "datagridheader",
                "template", "include",
            };
            for (const char* T : Tags) Lang.keywords.insert(T);

            // Colored as declarations so class and id stand out from arbitrary identifiers.
            static const char* const Attributes[] = {
                "id", "class", "style", "src", "href", "type", "name", "value",
                "checked", "disabled", "readonly", "selected", "for",
                "onclick", "onchange", "onsubmit", "onfocus", "onblur",
                "onmouseover", "onmouseout", "onmousedown", "onmouseup",
                "onkeydown", "onkeyup", "onload", "data-model", "data-bind",
            };
            for (const char* A : Attributes) Lang.declarations.insert(A);

            // RCSS properties (CSS subset + RmlUI extensions).
            static const char* const Properties[] = {
                // Layout
                "display", "position", "top", "right", "bottom", "left",
                "margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
                "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
                "width", "height", "min-width", "max-width", "min-height", "max-height",
                "box-sizing", "overflow", "overflow-x", "overflow-y", "z-index", "clip",
                // Flex
                "flex", "flex-direction", "flex-wrap", "flex-flow",
                "flex-grow", "flex-shrink", "flex-basis",
                "justify-content", "align-items", "align-self", "align-content", "gap",
                "row-gap", "column-gap",
                // Typography
                "font", "font-family", "font-size", "font-style", "font-weight",
                "line-height", "letter-spacing", "word-spacing",
                "text-align", "text-decoration", "text-transform", "white-space",
                "color", "opacity",
                // Background
                "background", "background-color", "background-image",
                // Borders
                "border", "border-color", "border-width", "border-style", "border-radius",
                "border-top", "border-right", "border-bottom", "border-left",
                "border-top-color", "border-right-color", "border-bottom-color", "border-left-color",
                "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
                "border-top-left-radius", "border-top-right-radius",
                "border-bottom-left-radius", "border-bottom-right-radius",
                // RmlUI-specific / animations
                "transition", "animation", "decorator", "font-effect",
                "perspective", "perspective-origin",
                "transform", "transform-origin",
                "image-color", "fill-image",
                "drag", "focus", "tab-index", "scrollbar-margin",
                "pointer-events", "cursor",
                "mix-blend-mode", "filter", "backdrop-filter",
                // Pseudo-property values used as identifiers in RCSS
                "none", "auto", "inherit", "initial",
                "block", "inline", "inline-block", "flex",
                "absolute", "relative", "fixed", "static",
                "row", "column", "row-reverse", "column-reverse",
                "wrap", "nowrap", "wrap-reverse",
                "flex-start", "flex-end", "center", "space-between", "space-around", "space-evenly",
                "stretch", "baseline",
                "hidden", "visible", "scroll",
                "bold", "italic", "normal", "underline",
            };
            for (const char* P : Properties) Lang.identifiers.insert(P);

            Initialized = true;
            return &Lang;
        }
    }

    FRmlUiEditorTool::FRmlUiEditorTool(IEditorToolContext* Context, FStringView InVirtualPath)
        : FAssetEditorTool(Context, DisplayNameFromPath(InVirtualPath))
        , VirtualPath(InVirtualPath.data(), InVirtualPath.size())
    {
        const FStringView ParentView = VFS::Parent(InVirtualPath, true);
        ParentDir = FString(ParentView.data(), ParentView.size());

        bIsStylesheet = (VirtualPath.size() >= 5) &&
            (FStringView(VirtualPath.c_str(), VirtualPath.size()).substr(VirtualPath.size() - 5) == FStringView(".rcss"));

        PullSettings();
    }

    void FRmlUiEditorTool::PullSettings()
    {
        // Syntax colors are read straight from the CDO, so they are not mirrored into members here.
        const CRmlUiEditorSettings* Settings = GetDefault<CRmlUiEditorSettings>();
        EditorFontScale         = Settings->FontScale;
        EditorTabSize           = Math::Max(1, Math::Min(8, Settings->TabSize));
        EditorLineSpacing       = Settings->LineSpacing;
        bEditorShowWhitespace   = Settings->bShowWhitespace;
        bEditorShowLineNumbers  = Settings->bShowLineNumbers;
        bEditorShowMiniMap      = Settings->bShowMiniMap;
        bAutoIndent             = Settings->bAutoIndent;
        bShowMatchingBrackets   = Settings->bMatchBrackets;
        bCompletePairedGlyphs   = Settings->bCompletePairs;
        bInsertSpacesOnTabs     = Settings->bInsertSpacesOnTabs;
        bTrimTrailingOnSave     = Settings->bTrimTrailingOnSave;
        bAutoReload             = Settings->bAutoReload;
        EditorPalette = (Settings->Palette == "Light") ? EPalette::Light : EPalette::Dark;
    }

    void FRmlUiEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        ApplyEditorSettings();
        CodeEditor.SetLanguage(GetRmlLanguage(bIsStylesheet));
        LoadFromDisk();

        // Retarget our path when the file is renamed/moved so a later save hits the new file.
        FileRenamedHandle = FCoreDelegates::OnContentFileRenamed.AddLambda([this](FStringView Old, FStringView New)
        {
            if (Old != FStringView(VirtualPath.c_str(), VirtualPath.size()))
            {
                return;
            }
            VirtualPath.assign(New.data(), New.size());
            const FStringView ParentView = VFS::Parent(New, true);
            ParentDir.assign(ParentView.data(), ParentView.size());
        });

        // Live-refreshes so palette and font tweaks apply without reopening the editor.
        SettingsSavedHandle = FCoreDelegates::OnSettingsSaved.AddLambda([this](CClass* Class)
        {
            if (Class == CRmlUiEditorSettings::StaticClass())
            {
                PullSettings();
                ApplyEditorSettings();
            }
        });

        char NameBuf[96];
        std::snprintf(NameBuf, sizeof(NameBuf), "rml_editor_%p", static_cast<void*>(this));

        const FUIntVector2 InitialSize{1280u, 720u};
        PreviewContext = RmlUi::CreateEditorContext(NameBuf, InitialSize);
        if (PreviewContext == nullptr)
        {
            LOG_ERROR("[RmlUiEditor] Failed to create preview context for '{}'.", VirtualPath.c_str());
        }
        else
        {
            RmlUi::SetEditorContextDpiScale(PreviewContext, PreviewDpiScale);
            // Start with a transparent clear so checker/solid bg can show through.
            RmlUi::SetEditorContextClearColor(PreviewContext, FVector4(0.0f, 0.0f, 0.0f, 0.0f));
        }

        ReloadDocument();
        StartWatching();

        // Kept long enough that typing a path does not reload mid-word, and ignores programmatic SetText.
        CodeEditor.SetChangeCallback([this]
        {
            const std::string Current = CodeEditor.GetText();
            if (Current == LastSyncedText)
            {
                return;
            }
            bBufferDirty = true;
            if (bAutoReload)
            {
                ReloadDocument();
            }
        }, /*delay ms*/ 900);

        CreateToolWindow(RmlEditorWindowName, [this](bool bFocused)
        {
            DrawEditorToolbar();
            ImGui::Separator();

            const ImVec2 Avail = ImGui::GetContentRegionAvail();
            const float StatusBarHeight = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
            const ImVec2 EditorSize(Avail.x, Math::Max(32.0f, Avail.y - StatusBarHeight));

            // Steals the wheel so TextEditor does not also use it for vertical scroll.
            const ImVec2 EditorMin = ImGui::GetCursorScreenPos();
            const ImVec2 EditorMax(EditorMin.x + EditorSize.x, EditorMin.y + EditorSize.y);
            ImGuiIO& Io = ImGui::GetIO();
            if (Io.KeyCtrl && Io.MouseWheel != 0.0f && ImGui::IsMouseHoveringRect(EditorMin, EditorMax))
            {
                EditorFontScale = Math::Clamp(EditorFontScale * (1.0f + Io.MouseWheel * 0.1f), 0.5f, 4.0f);
                Io.MouseWheel = 0.0f;
            }

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Mono);
            ImGui::PushFontSize(ImGui::GetStyle().FontSizeBase * EditorFontScale);
            CodeEditor.Render("##rml_text", EditorSize);
            ImGui::PopFontSize();
            ImGuiX::Font::PopFont();

            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            {
                HandleEditorShortcuts();
            }

            DrawEditorStatusBar();
        });

        CreateToolWindow(RmlPreviewWindowName, [this](bool bFocused)
        {
            DrawPreviewToolbar();
            ImGui::Separator();
            DrawPreviewCanvas();
        });

        HierarchyContext.IndentPerDepth = 14.0f;

        HierarchyContext.RebuildTreeFunction = [this](FTreeListView& Tree)
        {
            RebuildHierarchyTree(Tree);
        };

        // The assigned-widget name is a chip rather than part of the label, so it does not widen the search.
        HierarchyContext.FilterFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            if (HierarchySearch[0] == '\0')
            {
                return true;
            }

            const FRmlHierarchyItem& Data = Tree.Get<FRmlHierarchyItem>(Item);
            return ContainsCI(FString(Data.Tag.c_str(), Data.Tag.size()), HierarchySearch)
                || (!Data.Id.empty() && ContainsCI(FString(Data.Id.c_str(), Data.Id.size()), HierarchySearch));
        };

        HierarchyContext.ItemSelectedFunction = [this](FTreeListView& Tree, FTreeNodeID Item, bool)
        {
            if (!Item.IsValid())
            {
                return;
            }

            const FRmlHierarchyItem& Data = Tree.Get<FRmlHierarchyItem>(Item);

            // The open-tag offset addresses an element the live DOM cannot, since only ids reach a slot.
            SelectedSlotId = Data.bIsBody ? FString() : FString(Data.Id.c_str(), Data.Id.size());
            SelectedTag    = Data.bIsBody ? FString("body") : FString(Data.Tag.c_str(), Data.Tag.size());
            SelectedOpenLt = Data.bIsBody ? ~size_t(0) : Data.OpenLt;
        };

        HierarchyContext.HoveredFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            const FRmlHierarchyItem& Data = Tree.Get<FRmlHierarchyItem>(Item);
            if (!Data.bIsBody && !Data.Id.empty())
            {
                PendingHoveredSlotId = FString(Data.Id.c_str(), Data.Id.size());
            }
        };

        CreateToolWindow(RmlHierarchyWindowName, [this](bool bFocused)
        {
            DrawHierarchyPanel();
        });

        CreateToolWindow(RmlInspectorWindowName, [this](bool bFocused)
        {
            DrawInspectorPanel();
        });
    }

    void FRmlUiEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        FCoreDelegates::OnContentFileRenamed.Remove(FileRenamedHandle);
        FCoreDelegates::OnSettingsSaved.Remove(SettingsSavedHandle);
        FileWatcher.Stop();
        TearDownPreview();
    }

    void FRmlUiEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FAssetEditorTool::Update(UpdateContext);

        // Promoted here because the hierarchy publishes its hover a frame before the overlay reads it.
        HoveredSlotId = PendingHoveredSlotId;
        PendingHoveredSlotId.clear();

        if (bExternalChangePending.exchange(false, Atomic::MemoryOrderAcquire))
        {
            if (!bBufferDirty)
            {
                // Someone saved the file in another editor, which is as deliberate an act as saving here.
                LoadFromDisk();
                ReloadDocument(/*bAlwaysReport*/ true);
            }
            else
            {
                LOG_WARN("[RmlUiEditor] '{}' changed on disk but buffer is dirty; ignoring.", VirtualPath.c_str());
            }
        }

        RefreshCompositionSlots();
    }

    void FRmlUiEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("RmlUi",
            "Lumina ships RmlUi as its HTML/CSS-style markup layer. Documents live as plain .rml files "
            "alongside their .rcss stylesheets, no asset packaging.");
        DrawHelpTextRow("Live Preview",
            "Saving (Ctrl+S) reloads the document on the right pane. Auto Reload watches the file on disk "
            "and refreshes when external editors save.");
        DrawHelpTextRow("Decorators",
            "FRmlUiRenderer supports CPU gradient decorators (horizontal-gradient / vertical-gradient) but NOT "
            "shader-backed ones (linear-gradient, radial-gradient). Use the supported names.");
        DrawHelpTextRow("Resolution / Safe Zones",
            "Use the toolbar to lock canvas size to a target resolution. Safe zone overlays help align "
            "controls on TVs / consoles where overscan trims the edges.");
    }

    void FRmlUiEditorTool::OnSave()
    {
        if (bTrimTrailingOnSave)
        {
            CodeEditor.StripTrailingWhitespaces();
        }

        const std::string Body = CodeEditor.GetText();
        const FStringView View(Body.data(), Body.size());

        if (!VFS::WriteFile(FStringView(VirtualPath.c_str(), VirtualPath.size()), View))
        {
            ImGuiX::Notifications::NotifyError("Failed to save '{0}'.", VirtualPath.c_str());
            return;
        }

        LastSyncedText = Body;
        bBufferDirty = false;
        ImGuiX::Notifications::NotifySuccess("Saved '{0}'.", VirtualPath.c_str());

        ReloadDocument(/*bAlwaysReport*/ true);
    }

    void FRmlUiEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID LeftDockID = 0, RightDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.6f, &RightDockID, &LeftDockID);

        // Carve the designer column off the far right of the preview half.
        ImGuiID DesignerDockID = 0, PreviewDockID = 0;
        ImGui::DockBuilderSplitNode(RightDockID, ImGuiDir_Right, 0.34f, &DesignerDockID, &PreviewDockID);

        ImGuiID HierarchyDockID = 0, InspectorDockID = 0;
        ImGui::DockBuilderSplitNode(DesignerDockID, ImGuiDir_Down, 0.45f, &InspectorDockID, &HierarchyDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(RmlEditorWindowName).c_str(), LeftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(RmlPreviewWindowName).c_str(), PreviewDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(RmlHierarchyWindowName).c_str(), HierarchyDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(RmlInspectorWindowName).c_str(), InspectorDockID);
    }

    void FRmlUiEditorTool::ApplyEditorSettings()
    {
        CodeEditor.SetTabSize(Math::Max(1, Math::Min(8, EditorTabSize)));
        CodeEditor.SetInsertSpacesOnTabs(bInsertSpacesOnTabs);
        CodeEditor.SetLineSpacing(EditorLineSpacing);
        CodeEditor.SetShowWhitespacesEnabled(bEditorShowWhitespace);
        CodeEditor.SetShowLineNumbersEnabled(bEditorShowLineNumbers);
        CodeEditor.SetShowScrollbarMiniMapEnabled(bEditorShowMiniMap);
        CodeEditor.SetReadOnlyEnabled(bEditorReadOnly);
        CodeEditor.SetAutoIndentEnabled(bAutoIndent);
        CodeEditor.SetShowMatchingBrackets(bShowMatchingBrackets);
        CodeEditor.SetCompletePairedGlyphs(bCompletePairedGlyphs);

        // Starts from the chosen base chrome, then overrides syntax slots from CRmlUiEditorSettings.
        TextEditor::Palette Pal = (EditorPalette == EPalette::Dark)
            ? TextEditor::GetDarkPalette()
            : TextEditor::GetLightPalette();

        const CRmlUiEditorSettings* Colors = GetDefault<CRmlUiEditorSettings>();
        auto Set = [&Pal](TextEditor::Color Slot, const FVector3& C)
        {
            const auto B = [](float V) { return (int)(Math::Clamp(V, 0.0f, 1.0f) * 255.0f + 0.5f); };
            Pal[(size_t)Slot] = IM_COL32(B(C.x), B(C.y), B(C.z), 255);
        };
        Set(TextEditor::Color::keyword,         Colors->TagColor);
        Set(TextEditor::Color::declaration,     Colors->AttributeColor);
        Set(TextEditor::Color::knownIdentifier, Colors->PropertyColor);
        Set(TextEditor::Color::identifier,      Colors->IdentifierColor);
        Set(TextEditor::Color::number,          Colors->NumberColor);
        Set(TextEditor::Color::string,          Colors->StringColor);
        Set(TextEditor::Color::comment,         Colors->CommentColor);
        Set(TextEditor::Color::punctuation,     Colors->PunctuationColor);
        CodeEditor.SetPalette(Pal);
    }

    void FRmlUiEditorTool::PersistSettings() const
    {
        CRmlUiEditorSettings* Settings = GetMutableDefault<CRmlUiEditorSettings>();
        Settings->FontScale             = EditorFontScale;
        Settings->TabSize               = EditorTabSize;
        Settings->LineSpacing           = EditorLineSpacing;
        Settings->bShowWhitespace       = bEditorShowWhitespace;
        Settings->bShowLineNumbers      = bEditorShowLineNumbers;
        Settings->bShowMiniMap          = bEditorShowMiniMap;
        Settings->bAutoIndent           = bAutoIndent;
        Settings->bMatchBrackets        = bShowMatchingBrackets;
        Settings->bCompletePairs        = bCompletePairedGlyphs;
        Settings->bInsertSpacesOnTabs   = bInsertSpacesOnTabs;
        Settings->bTrimTrailingOnSave   = bTrimTrailingOnSave;
        Settings->bAutoReload           = bAutoReload;
        Settings->Palette               = (EditorPalette == EPalette::Dark) ? "Dark" : "Light";
        GConfig->SaveSettings(CRmlUiEditorSettings::StaticClass());
    }


    void FRmlUiEditorTool::DrawEditorToolbar()
    {
        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", VirtualPath.c_str());
        ImGuiX::Font::PopFont();

        if (bBufferDirty)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "  *unsaved");
        }

        ImGui::Spacing();

        if (ImGui::Button(LE_ICON_CONTENT_SAVE " Save"))
        {
            OnSave();
        }
        ImGuiX::TextTooltip("Write the buffer to disk (Ctrl+S).");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_REFRESH " Reload"))
        {
            LoadFromDisk();
            ReloadDocument(/*bAlwaysReport*/ true);
        }
        ImGuiX::TextTooltip("Discard buffer changes and reload from disk.");

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        ImGui::BeginDisabled(!CodeEditor.CanUndo());
        if (ImGui::Button(LE_ICON_UNDO_VARIANT " Undo")) CodeEditor.Undo();
        ImGuiX::TextTooltip("Undo last change (Ctrl+Z).");
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!CodeEditor.CanRedo());
        if (ImGui::Button(LE_ICON_REDO_VARIANT " Redo")) CodeEditor.Redo();
        ImGuiX::TextTooltip("Redo last undone change (Ctrl+Y).");
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        if (ImGui::Button(LE_ICON_PLAY " Re-render"))
        {
            ReloadDocument(/*bAlwaysReport*/ true);
        }
        ImGuiX::TextTooltip("Re-parse the current buffer into the preview.");

        ImGui::SameLine();
        ImGui::Checkbox("Auto", &bAutoReload);
        ImGuiX::TextTooltip("Re-parse the buffer ~250ms after each edit.");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_MAGNIFY " Find"))
        {
            CodeEditor.OpenFindReplaceWindow();
        }
        ImGuiX::TextTooltip("Open the find/replace bar (Ctrl+F).");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_FORMAT_LINE_SPACING " Goto"))
        {
            bRequestOpenGoto = true;
        }
        ImGuiX::TextTooltip("Jump to a specific line number (Ctrl+G).");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CODE_BRACES " Snippets"))
        {
            ImGui::OpenPopup("##rml_snippets");
        }
        ImGuiX::TextTooltip("Insert a tag or RCSS rule at the cursor.");
        DrawSnippetsPopup();

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_AUTO_FIX " Format"))
        {
            ImGui::OpenPopup("##rml_format");
        }
        ImGuiX::TextTooltip("Whitespace and case transforms.");
        DrawFormatPopup();

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        if (ImGui::Button(LE_ICON_HELP_CIRCLE " Help"))
        {
            ImGui::OpenPopup("##rml_help");
        }
        ImGuiX::TextTooltip("Quick RML / RCSS reference.");
        DrawHelpPopup();

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_COG " Settings"))
        {
            ImGui::OpenPopup("##rml_editor_settings");
        }

        if (ImGui::BeginPopup("##rml_editor_settings"))
        {
            bool bDirty = false;

            ImGui::TextDisabled("Display");
            ImGui::Separator();

            ImGui::SliderFloat("Font scale", &EditorFontScale, 0.75f, 3.0f, "%.2fx");
            ImGuiX::TextTooltip("Tip: Ctrl+wheel over the editor adjusts this live.");

            if (ImGui::SliderFloat("Line spacing", &EditorLineSpacing, 1.0f, 2.0f, "%.2f")) bDirty = true;
            if (ImGui::SliderInt("Tab size", &EditorTabSize, 1, 8)) bDirty = true;
            if (ImGui::Checkbox("Show line numbers",      &bEditorShowLineNumbers)) bDirty = true;
            if (ImGui::Checkbox("Show whitespace",        &bEditorShowWhitespace))  bDirty = true;
            if (ImGui::Checkbox("Show scrollbar minimap", &bEditorShowMiniMap))     bDirty = true;
            if (ImGui::Checkbox("Read-only",              &bEditorReadOnly))        bDirty = true;

            ImGui::Spacing();
            ImGui::TextDisabled("Editing");
            ImGui::Separator();
            if (ImGui::Checkbox("Auto-indent",          &bAutoIndent))           bDirty = true;
            if (ImGui::Checkbox("Match brackets",       &bShowMatchingBrackets)) bDirty = true;
            if (ImGui::Checkbox("Auto-close pairs",     &bCompletePairedGlyphs)) bDirty = true;
            if (ImGui::Checkbox("Insert spaces on Tab", &bInsertSpacesOnTabs))   bDirty = true;
            ImGuiX::TextTooltip("When on, pressing Tab inserts spaces instead of a tab character.");
            ImGui::Checkbox("Trim trailing whitespace on save", &bTrimTrailingOnSave);

            ImGui::Spacing();
            ImGui::TextDisabled("Theme");
            ImGui::Separator();

            int PaletteIdx = (int)EditorPalette;
            const char* PaletteLabels[] = { "Dark", "Light" };
            if (ImGui::Combo("Palette", &PaletteIdx, PaletteLabels, IM_ARRAYSIZE(PaletteLabels)))
            {
                EditorPalette = (EPalette)PaletteIdx;
                bDirty = true;
            }

            // Saving fires the live-refresh, so the editor recolors immediately.
            if (ImGui::Button(LE_ICON_DICE_5 " Randomize colors", ImVec2(-1, 0)))
            {
                CRmlUiEditorSettings* Colors = GetMutableDefault<CRmlUiEditorSettings>();
                Colors->TagColor         = RandomVibrantColor();
                Colors->AttributeColor   = RandomVibrantColor();
                Colors->PropertyColor    = RandomVibrantColor();
                Colors->IdentifierColor  = RandomVibrantColor();
                Colors->NumberColor      = RandomVibrantColor();
                Colors->StringColor      = RandomVibrantColor();
                Colors->CommentColor     = RandomVibrantColor();
                Colors->PunctuationColor = RandomVibrantColor();
                GConfig->SaveSettings(CRmlUiEditorSettings::StaticClass());
            }
            ImGuiX::TextTooltip("Roll a random vibrant set of syntax colors.");

            ImGui::Spacing();
            ImGui::Separator();

            if (ImGui::Button("Persist as default", ImVec2(-1, 0)))
            {
                PersistSettings();
                ImGuiX::Notifications::NotifySuccess("RmlUi editor settings saved.");
            }

            if (ImGui::Button("Reset to defaults", ImVec2(-1, 0)))
            {
                EditorFontScale = 1.25f;
                EditorTabSize = 4;
                EditorLineSpacing = 1.0f;
                bEditorShowWhitespace = false;
                bEditorShowLineNumbers = true;
                bEditorShowMiniMap = true;
                bEditorReadOnly = false;
                bAutoIndent = true;
                bShowMatchingBrackets = true;
                bCompletePairedGlyphs = true;
                bInsertSpacesOnTabs = false;
                bTrimTrailingOnSave = false;
                EditorPalette = EPalette::Dark;
                bDirty = true;
            }

            if (bDirty)
            {
                ApplyEditorSettings();
            }

            ImGui::EndPopup();
        }

        if (bRequestOpenGoto)
        {
            ImGui::OpenPopup("##rml_goto_line");
            bRequestOpenGoto = false;
            GotoLineBuffer = CodeEditor.GetCurrentCursorPosition().line + 1;
        }
        DrawGotoLinePopup();
    }

    void FRmlUiEditorTool::DrawSnippetsPopup()
    {
        if (!ImGui::BeginPopup("##rml_snippets"))
        {
            return;
        }

        ImGui::TextDisabled("RML elements");
        ImGui::Separator();
        for (const FRmlSnippet& Snip : kRmlDocSnippets)
        {
            if (ImGui::MenuItem(Snip.Label))
            {
                InsertSnippet(Snip.Body);
            }
        }

        ImGui::Spacing();
        ImGui::TextDisabled("RCSS rules");
        ImGui::Separator();
        for (const FRmlSnippet& Snip : kRcssSnippets)
        {
            if (ImGui::MenuItem(Snip.Label))
            {
                InsertSnippet(Snip.Body);
            }
        }

        ImGui::EndPopup();
    }

    void FRmlUiEditorTool::InsertSnippet(const char* Snippet)
    {
        if (Snippet == nullptr || *Snippet == '\0')
        {
            return;
        }
        CodeEditor.ReplaceTextInCurrentCursor(std::string_view(Snippet));
        CodeEditor.SetFocus();
    }

    void FRmlUiEditorTool::DrawFormatPopup()
    {
        if (!ImGui::BeginPopup("##rml_format"))
        {
            return;
        }

        ImGui::TextDisabled("Document");
        ImGui::Separator();
        if (ImGui::MenuItem("Strip trailing whitespace")) CodeEditor.StripTrailingWhitespaces();
        if (ImGui::MenuItem("Tabs to spaces"))            CodeEditor.TabsToSpaces();
        if (ImGui::MenuItem("Spaces to tabs"))            CodeEditor.SpacesToTabs();

        ImGui::Spacing();
        ImGui::TextDisabled("Selection");
        ImGui::Separator();
        const bool bHasSel = CodeEditor.AnyCursorHasSelection();
        ImGui::BeginDisabled(!bHasSel);
        if (ImGui::MenuItem("Indent",       "Tab"))        CodeEditor.IndentLines();
        if (ImGui::MenuItem("Deindent",     "Shift+Tab"))  CodeEditor.DeindentLines();
        if (ImGui::MenuItem("Move up",      "Alt+Up"))     CodeEditor.MoveUpLines();
        if (ImGui::MenuItem("Move down",    "Alt+Down"))   CodeEditor.MoveDownLines();
        if (ImGui::MenuItem("To upper case"))               CodeEditor.SelectionToUpperCase();
        if (ImGui::MenuItem("To lower case"))               CodeEditor.SelectionToLowerCase();
        ImGui::EndDisabled();

        ImGui::EndPopup();
    }

    void FRmlUiEditorTool::DrawGotoLinePopup()
    {
        if (!ImGui::BeginPopup("##rml_goto_line"))
        {
            return;
        }

        ImGui::TextDisabled("Goto line (1 - %d)", CodeEditor.GetLineCount());
        ImGui::Separator();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SetKeyboardFocusHere();
        const bool bSubmit = ImGui::InputInt("##rml_goto_input", &GotoLineBuffer, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue);

        ImGui::SameLine();
        const bool bGo = ImGui::Button("Go");

        if (bSubmit || bGo)
        {
            const int Target = Math::Max(1, Math::Min(CodeEditor.GetLineCount(), GotoLineBuffer)) - 1;
            CodeEditor.SetCursor(Target, 0);
            CodeEditor.ScrollToLine(Target, TextEditor::Scroll::alignMiddle);
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    void FRmlUiEditorTool::DrawHelpPopup()
    {
        if (!ImGui::BeginPopup("##rml_help"))
        {
            return;
        }

        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "RML / RCSS Quick Reference");
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Editor shortcuts", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::BeginTable("##rml_help_keys", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
            {
                auto Row = [&](const char* Key, const char* Desc)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "%s", Key);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(Desc);
                };
                Row("Ctrl+S",        "Save");
                Row("Ctrl+F",        "Find / replace");
                Row("Ctrl+G",        "Goto line");
                Row("Ctrl+Z / Y",    "Undo / redo");
                Row("Ctrl+Wheel",    "Zoom font (in editor)");
                Row("Tab / Shift+Tab","Indent / deindent selection");
                Row("Alt+Up / Down", "Move line(s) up/down");
                ImGui::EndTable();
            }
        }
        if (ImGui::CollapsingHeader("Preview shortcuts"))
        {
            if (ImGui::BeginTable("##rml_help_preview", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
            {
                auto Row = [&](const char* Key, const char* Desc)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "%s", Key);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(Desc);
                };
                Row("Ctrl+Wheel",  "Zoom canvas (centered on mouse)");
                Row("Middle-drag", "Pan");
                Row("Double-click","Reset view");
                ImGui::EndTable();
            }
        }
        if (ImGui::CollapsingHeader("RML basics"))
        {
            ImGui::TextWrapped(
                "<rml> is the document root.\n"
                "<head> holds <title>, <style>, <link>; <body> holds visible elements.\n"
                "Inline RCSS goes inside a <style> block; external sheets via:\n"
                "    <link rel=\"stylesheet\" type=\"text/rcss\" href=\"...\"/>\n"
                "Use id=\"\" / class=\"\" to target with selectors. data-model / data-bind\n"
                "drive Rml's data binding system.");
        }
        if (ImGui::CollapsingHeader("RCSS units"))
        {
            ImGui::TextWrapped(
                "px - raw pixels\n"
                "dp - density-independent (scales with DPI slider)\n"
                "%%  - percentage of parent\n"
                "em - relative to current font size\n"
                "vw / vh - viewport width / height percent");
        }
        if (ImGui::CollapsingHeader("Layout, flex"))
        {
            ImGui::TextWrapped(
                "display: flex;\n"
                "flex-direction: row | column;\n"
                "justify-content: flex-start | center | space-between | ...\n"
                "align-items: stretch | center | flex-start | ...\n"
                "gap: 8dp;\n"
                "Children: flex: 1; flex-grow / flex-shrink / flex-basis.");
        }
        if (ImGui::CollapsingHeader("Decorators & font effects"))
        {
            ImGui::TextWrapped(
                "decorator: image( url );\n"
                "decorator: gradient( vertical #1f2a36 #0e1620 );\n"
                "decorator: tiled-box( ... );\n"
                "font-effect: outline(1dp #000);\n"
                "font-effect: shadow(0dp 1dp #000a);\n"
                "font-effect: glow(2dp 0dp 0dp #4af);");
        }
        if (ImGui::CollapsingHeader("Pseudo-classes"))
        {
            ImGui::TextWrapped(
                ":hover :active :focus :checked :disabled\n"
                ":nth-child(n) :first-child :last-child\n"
                "Combine: button:hover.primary { ... }");
        }
        if (ImGui::CollapsingHeader("Color literals"))
        {
            ImGui::TextWrapped(
                "#rgb / #rgba / #rrggbb / #rrggbbaa hex literals.\n"
                "Click any hex literal in the editor to pop the color picker.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Editor auto-reloads the preview ~250ms after each edit.");
        ImGui::EndPopup();
    }

    void FRmlUiEditorTool::HandleEditorShortcuts()
    {
        // Rebindable via CRmlUiEditorSettings > Hotkeys.
        const CRmlUiEditorSettings* Keys = GetDefault<CRmlUiEditorSettings>();
        if (ImGuiX::IsChordPressed(Keys->GoToLineKey))
        {
            bRequestOpenGoto = true;
        }
    }

    void FRmlUiEditorTool::DrawEditorStatusBar()
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(20, 20, 25, 200));
        if (ImGui::BeginChild("##rml_editor_status", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            const TextEditor::CursorPosition Pos = CodeEditor.GetCurrentCursorPosition();
            const int LineCount = CodeEditor.GetLineCount();

            // GetText() copies the whole document, so recompute only when the undo index moves.
            const size_t Undo = CodeEditor.GetUndoIndex();
            if (Undo != CachedStatusUndoIndex)
            {
                CachedDocBytes = CodeEditor.GetText().size();
                CachedStatusUndoIndex = Undo;
            }
            const size_t Bytes = CachedDocBytes;

            ImGui::Text("Ln %d, Col %d", Pos.line + 1, Pos.column + 1);

            if (CodeEditor.AnyCursorHasSelection())
            {
                const TextEditor::CursorSelection Sel = CodeEditor.GetMainCursorSelection();
                const std::string SelText = CodeEditor.GetSectionText(Sel.start.line, Sel.start.column, Sel.end.line, Sel.end.column);
                const int SelLines = (Sel.end.line - Sel.start.line) + 1;
                ImGui::SameLine(0, 12);
                ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.45f, 1.0f), "(sel %zu chars / %d lines)", SelText.size(), SelLines);
            }

            ImGui::SameLine(0, 20);
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.65f, 1.0f), "%d lines", LineCount);
            ImGui::SameLine(0, 20);
            if (Bytes >= 1024)
            {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.65f, 1.0f), "%.1f KB", float(Bytes) / 1024.0f);
            }
            else
            {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.65f, 1.0f), "%zu B", Bytes);
            }

            ImGui::SameLine(0, 20);
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "RML/RCSS");

            ImGui::SameLine(0, 20);
            ImGui::TextColored(ImVec4(0.55f, 0.65f, 0.85f, 1.0f), "%s : %d",
                bInsertSpacesOnTabs ? "Spaces" : "Tabs", EditorTabSize);
            ImGuiX::TextTooltip("Indent mode and tab size. Toggle in Settings.");

            if (bEditorReadOnly)
            {
                ImGui::SameLine(0, 20);
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.25f, 1.0f), "READ-ONLY");
            }

            if (bBufferDirty)
            {
                ImGui::SameLine(0, 20);
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "modified");
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }


    void FRmlUiEditorTool::DrawPreviewToolbar()
    {
        // Resolution preset.
        ImGui::TextUnformatted("Canvas:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);

        const char* Items[IM_ARRAYSIZE(ResolutionPresets)];
        for (int i = 0; i < IM_ARRAYSIZE(ResolutionPresets); ++i) Items[i] = ResolutionPresets[i].Label;

        if (ImGui::Combo("##rml_resolution", &ResolutionPreset, Items, IM_ARRAYSIZE(Items)))
        {
            if (ResolutionPreset != CustomPresetIndex)
            {
                CanvasWidth = ResolutionPresets[ResolutionPreset].Width;
                CanvasHeight = ResolutionPresets[ResolutionPreset].Height;
            }
        }
        ImGuiX::TextTooltip("Render canvas resolution. The preview pane scales the canvas to fit; use View Zoom for 1:1 inspection.");

        if (ResolutionPreset == CustomPresetIndex)
        {
            ImGui::SameLine();
            int W = (int)CanvasWidth, H = (int)CanvasHeight;
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::DragInt("##rml_w", &W, 4.0f, 16, 7680, "W %d")) CanvasWidth  = (uint32)Math::Max(16, W);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::DragInt("##rml_h", &H, 4.0f, 16, 4320, "H %d")) CanvasHeight = (uint32)Math::Max(16, H);
        }

        // Only meaningful when both dimensions are set.
        if (CanvasWidth > 0 && CanvasHeight > 0)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(LE_ICON_PHONE_ROTATE_PORTRAIT "##rml_swap"))
            {
                std::swap(CanvasWidth, CanvasHeight);
                ResolutionPreset = CustomPresetIndex;
            }
            ImGuiX::TextTooltip("Swap width and height (portrait/landscape).");
        }

        ImGui::SameLine();
        ImGui::TextUnformatted("|  DPI:");
        ImGui::SameLine();
        ImGui::Checkbox("Auto##rml_dpi_auto", &bAutoDpi);
        ImGuiX::TextTooltip("Match the engine dp convention (canvas height / 1080) so dp-sized UI previews at in-game scale.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::BeginDisabled(bAutoDpi);
        if (ImGui::SliderFloat("##rml_dpi", &PreviewDpiScale, 0.25f, 4.0f, "%.2fx"))
        {
            if (PreviewContext != nullptr) RmlUi::SetEditorContextDpiScale(PreviewContext, PreviewDpiScale);
        }
        ImGui::EndDisabled();
        ImGuiX::TextTooltip("Density-independent pixel ratio. Turn off Auto to set it manually.");

        ImGui::SameLine();
        ImGui::TextUnformatted("|  View:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::SliderFloat("##rml_view", &ViewZoom, 0.1f, 4.0f, "%.2fx");
        ImGuiX::TextTooltip("Pan with middle-drag, zoom with Ctrl+wheel, double-click to reset.");

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset View"))
        {
            ViewZoom = 1.0f;
            ViewPan = ImVec2(0, 0);
        }

        // Second row.
        ImGui::Spacing();

        if (ImGui::Button(LE_ICON_PALETTE " Background"))
        {
            ImGui::OpenPopup("##rml_bg_popup");
        }
        if (ImGui::BeginPopup("##rml_bg_popup"))
        {
            int Mode = (int)BgMode;
            const char* Modes[] = { "Checker", "Solid", "Transparent" };
            if (ImGui::Combo("Mode", &Mode, Modes, IM_ARRAYSIZE(Modes))) BgMode = (EBgMode)Mode;
            if (BgMode == EBgMode::Solid)
            {
                ImGui::ColorEdit4("Color", &BgColor.x, ImGuiColorEditFlags_NoInputs);
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::Checkbox("Grid", &bShowGrid);
        if (bShowGrid)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f);
            ImGui::DragFloat("##rml_grid_size", &GridSize, 1.0f, 4.0f, 512.0f, "%.0fpx");
            ImGui::SameLine();
            ImGui::ColorEdit4("##rml_grid_color", &GridColor.x, ImGuiColorEditFlags_NoInputs);
            ImGuiX::TextTooltip("Canvas-space grid for layout alignment.");
        }

        ImGui::SameLine();
        ImGui::TextUnformatted("|");
        ImGui::SameLine();
        ImGui::Checkbox("Safe zones", &bShowSafeZones);
        if (bShowSafeZones)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::SliderFloat("##rml_safe_action", &SafeZoneAction, 0.50f, 1.0f, "Act %.2f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::SliderFloat("##rml_safe_title", &SafeZoneTitle, 0.50f, 1.0f, "Tit %.2f");
            ImGui::SameLine();
            ImGui::ColorEdit4("##rml_safe_color", &SafeZoneColor.x, ImGuiColorEditFlags_NoInputs);
            ImGuiX::TextTooltip("Inner rectangle: title-safe (text/HUD). Outer: action-safe (interactive elements).");
        }

        ImGui::SameLine();
        ImGui::TextUnformatted("|");
        ImGui::SameLine();
        ImGui::Checkbox("Rulers", &bShowRulers);
    }

    void FRmlUiEditorTool::DrawPreviewCanvas()
    {
        const ImVec2 Pane = ImGui::GetContentRegionAvail();
        if (Pane.x < 16.0f || Pane.y < 16.0f)
        {
            return;
        }

        if (PreviewContext == nullptr)
        {
            ImGui::TextDisabled("Preview unavailable.");
            return;
        }

        // Scrollable / pan child for the canvas.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(18, 18, 22, 255));
        ImGui::BeginChild("##rml_canvas_view", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);

        const ImVec2 PaneMin = ImGui::GetWindowPos();
        const ImVec2 PaneSize = ImGui::GetWindowSize();
        ImDrawList* DL = ImGui::GetWindowDrawList();

        if (ImGui::IsWindowHovered())
        {
            ImGuiIO& Io = ImGui::GetIO();
            if (Io.KeyCtrl && Io.MouseWheel != 0.0f)
            {
                const float Old = ViewZoom;
                ViewZoom = Math::Clamp(ViewZoom * (1.0f + Io.MouseWheel * 0.1f), 0.1f, 8.0f);
                // Pan-correct so zoom is centered on the mouse cursor.
                const ImVec2 Mouse = Io.MousePos;
                const ImVec2 Center(PaneMin.x + PaneSize.x * 0.5f, PaneMin.y + PaneSize.y * 0.5f);
                ViewPan.x = (ViewPan.x - (Mouse.x - Center.x)) * (ViewZoom / Old) + (Mouse.x - Center.x);
                ViewPan.y = (ViewPan.y - (Mouse.y - Center.y)) * (ViewZoom / Old) + (Mouse.y - Center.y);
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            {
                const ImVec2 D = Io.MouseDelta;
                ViewPan.x += D.x;
                ViewPan.y += D.y;
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                ViewZoom = 1.0f;
                ViewPan = ImVec2(0, 0);
            }
        }

        // Zero by zero means fit to pane, which adopts the pane's own aspect and never letterboxes.
        const uint32 EffW = (CanvasWidth  != 0 && CanvasHeight != 0) ? CanvasWidth  : (uint32)Math::Max(16.0f, PaneSize.x);
        const uint32 EffH = (CanvasWidth  != 0 && CanvasHeight != 0) ? CanvasHeight : (uint32)Math::Max(16.0f, PaneSize.y);

        // Fit the canvas inside the pane at View=1.0, then scale by ViewZoom.
        const float CanvasAspect = float(EffW) / float(EffH);
        const float PaneAspect   = PaneSize.x / Math::Max(1.0f, PaneSize.y);
        ImVec2 FitSize;
        if (CanvasAspect > PaneAspect)
        {
            FitSize.x = PaneSize.x;
            FitSize.y = PaneSize.x / CanvasAspect;
        }
        else
        {
            FitSize.y = PaneSize.y;
            FitSize.x = PaneSize.y * CanvasAspect;
        }

        // Rasterize at the size we DISPLAY at, so one context pixel is one screen pixel and never resamples.
        const float WantW = Math::Max(16.0f, std::floor(FitSize.x * ViewZoom));
        const float WantH = Math::Max(16.0f, std::floor(FitSize.y * ViewZoom));

        // Applied as ONE factor so the aspect is preserved, since clamping axes apart would stretch.
        const float MaxRasterW = Math::Min(4096.0f, Math::Max(PaneSize.x * 2.0f, 512.0f));
        const float MaxRasterH = Math::Min(4096.0f, Math::Max(PaneSize.y * 2.0f, 512.0f));
        const float Shrink     = Math::Min(1.0f, Math::Min(MaxRasterW / WantW, MaxRasterH / WantH));

        const int CanvasPxW = (int)Math::Max(16.0f, std::floor(WantW * Shrink));
        const int CanvasPxH = (int)Math::Max(16.0f, std::floor(WantH * Shrink));

        EnsurePreviewTarget((uint32)CanvasPxW, (uint32)CanvasPxH);

        // Keyed off the DISPLAYED height, which is what makes dp content occupy the right fraction.
        if (bAutoDpi && PreviewHeight > 0)
        {
            const float AutoDpi = Math::Clamp(float(PreviewHeight) / 1080.0f, 0.25f, 4.0f);
            if (std::abs(AutoDpi - PreviewDpiScale) > 0.001f)
            {
                PreviewDpiScale = AutoDpi;
                RmlUi::SetEditorContextDpiScale(PreviewContext, PreviewDpiScale);
            }
        }

        if (!PreviewTarget.IsValid())
        {
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Preview unavailable.");
            return;
        }

        // A checker or transparent clear leaves alpha at zero so ImGui composites the background below.
        FVector4 ClearColor;
        switch (BgMode)
        {
        case EBgMode::Solid:       ClearColor = FVector4(BgColor.x, BgColor.y, BgColor.z, 1.0f); break;
        case EBgMode::Checker:     // fallthrough, we draw the checker in ImGui below
        case EBgMode::Transparent: ClearColor = FVector4(0.0f, 0.0f, 0.0f, 0.0f); break;
        }
        RmlUi::SetEditorContextClearColor(PreviewContext, ClearColor);
        // The visible size drives both SetDimensions and the viewport, so layout matches what is sampled.
        RmlUi::SetEditorContextTarget(PreviewContext, PreviewTarget.Texture, FUIntVector2(PreviewWidth, PreviewHeight));

        // Unclamped the display rect IS the raster, and clamped the smaller raster scales up into it.
        const ImVec2 CanvasSize = (Shrink >= 1.0f)
            ? ImVec2((float)CanvasPxW, (float)CanvasPxH)
            : ImVec2(WantW, WantH);
        const ImVec2 PaneCenter(PaneMin.x + PaneSize.x * 0.5f, PaneMin.y + PaneSize.y * 0.5f);
        // Texel centers must land on pixel centers, or a half-pixel offset blurs text just as badly.
        const ImVec2 CanvasMin(
            std::floor(PaneCenter.x - CanvasSize.x * 0.5f + ViewPan.x),
            std::floor(PaneCenter.y - CanvasSize.y * 0.5f + ViewPan.y));
        const ImVec2 CanvasMax(CanvasMin.x + CanvasSize.x, CanvasMin.y + CanvasSize.y);

        if (BgMode == EBgMode::Checker)
        {
            const float Cell = 12.0f;
            const ImU32 A = IM_COL32(48, 48, 52, 255);
            const ImU32 B = IM_COL32(36, 36, 40, 255);
            // Clip to canvas rect.
            DL->PushClipRect(CanvasMin, CanvasMax, true);
            for (float y = CanvasMin.y; y < CanvasMax.y; y += Cell)
            {
                for (float x = CanvasMin.x; x < CanvasMax.x; x += Cell)
                {
                    const bool Even = (int((x - CanvasMin.x) / Cell) + int((y - CanvasMin.y) / Cell)) & 1;
                    DL->AddRectFilled(ImVec2(x, y), ImVec2(x + Cell, y + Cell), Even ? A : B);
                }
            }
            DL->PopClipRect();
        }
        else if (BgMode == EBgMode::Solid)
        {
            DL->AddRectFilled(CanvasMin, CanvasMax, ToU32(BgColor));
        }
        // Transparent, draw nothing, the pane background shows through.

        // The texture is padded up to a block so a resize drag reuses one allocation.
        const ImTextureID Tex = (ImTextureID)(uint64)PreviewTarget.SampledSlot;
        const ImVec2 Uv1(
            float(PreviewWidth)  / float(Math::Max(1u, PreviewRTWidth)),
            float(PreviewHeight) / float(Math::Max(1u, PreviewRTHeight)));
        DL->AddImage(Tex, CanvasMin, CanvasMax, ImVec2(0.0f, 0.0f), Uv1);
        DL->AddRect(CanvasMin, CanvasMax, IM_COL32(80, 80, 95, 255), 0.0f, 0, 1.0f);

        // The preview is only a texture, so scrollbars and hover states are dead until input is forwarded.
        {
            ImGuiIO& InputIo = ImGui::GetIO();
            const bool bOverCanvas = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
                && ImGui::IsMouseHoveringRect(CanvasMin, CanvasMax);

            if (bOverCanvas && !InputIo.KeyCtrl)
            {
                const float CanvasScale = (CanvasSize.x > 0.0f) ? (float(PreviewWidth) / CanvasSize.x) : 1.0f;
                const FVector2 Local(
                    (InputIo.MousePos.x - CanvasMin.x) * CanvasScale,
                    (InputIo.MousePos.y - CanvasMin.y) * CanvasScale);

                const bool bLeft  = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                const bool bRight = ImGui::IsMouseDown(ImGuiMouseButton_Right);

                RmlUi::ForwardEditorContextMouse(PreviewContext, Local, InputIo.MouseWheel,
                                                 bLeft, bRight, bPreviewLeftDown, bPreviewRightDown);

                bPreviewLeftDown  = bLeft;
                bPreviewRightDown = bRight;
                bPreviewHovered   = true;

                // Consumed here, or the pane scrolls behind the document.
                InputIo.MouseWheel = 0.0f;
            }
            else if (bPreviewHovered)
            {
                RmlUi::ForwardEditorContextMouseLeave(PreviewContext);
                bPreviewHovered   = false;
                bPreviewLeftDown  = false;
                bPreviewRightDown = false;
            }
        }

        // 1.0 by construction, but kept computed so the overlay math holds if the two ever diverge.
        const float ScalePx = CanvasSize.x / float(Math::Max(1u, PreviewWidth));

        if (bShowGrid && GridSize > 0.0f)
        {
            const ImU32 GridU = ToU32(GridColor);
            const float Step = GridSize * ScalePx;
            DL->PushClipRect(CanvasMin, CanvasMax, true);
            for (float x = CanvasMin.x + Step; x < CanvasMax.x; x += Step)
            {
                DL->AddLine(ImVec2(x, CanvasMin.y), ImVec2(x, CanvasMax.y), GridU);
            }
            for (float y = CanvasMin.y + Step; y < CanvasMax.y; y += Step)
            {
                DL->AddLine(ImVec2(CanvasMin.x, y), ImVec2(CanvasMax.x, y), GridU);
            }
            DL->PopClipRect();
        }

        if (bShowSafeZones)
        {
            const ImU32 SafeU = ToU32(SafeZoneColor);
            auto DrawSafe = [&](float Frac)
            {
                const ImVec2 Sz(CanvasSize.x * Frac, CanvasSize.y * Frac);
                const ImVec2 A(CanvasMin.x + (CanvasSize.x - Sz.x) * 0.5f,
                               CanvasMin.y + (CanvasSize.y - Sz.y) * 0.5f);
                const ImVec2 B(A.x + Sz.x, A.y + Sz.y);
                DL->AddRect(A, B, SafeU, 0.0f, 0, 1.5f);
            };
            DrawSafe(SafeZoneAction);
            DrawSafe(SafeZoneTitle);
        }

        if (bShowRulers)
        {
            // Tick marks every 100 canvas px along top + left edges.
            const ImU32 RU = IM_COL32(180, 180, 200, 200);
            DL->PushClipRect(PaneMin, ImVec2(PaneMin.x + PaneSize.x, PaneMin.y + PaneSize.y), true);
            const float TickStep = 100.0f * ScalePx;
            for (float x = CanvasMin.x; x <= CanvasMax.x; x += TickStep)
            {
                DL->AddLine(ImVec2(x, CanvasMin.y - 6.0f), ImVec2(x, CanvasMin.y), RU);
            }
            for (float y = CanvasMin.y; y <= CanvasMax.y; y += TickStep)
            {
                DL->AddLine(ImVec2(CanvasMin.x - 6.0f, y), ImVec2(CanvasMin.x, y), RU);
            }
            DL->PopClipRect();
        }

        // Slot composition overlays sit on top of everything else (and own their hit-testing).
        DrawSlotOverlays(CanvasMin, ScalePx);

        // HUD line (bottom-left).
        const float HudY = PaneMin.y + PaneSize.y - ImGui::GetTextLineHeightWithSpacing();
        DL->AddText(ImVec2(PaneMin.x + 8.0f, HudY),
                    IM_COL32(170, 170, 190, 220),
                    [&]
                    {
                        // Both sizes, since the layout resolution and the rasterized pixel count now differ.
                        static char Buf[160];
                        std::snprintf(Buf, sizeof(Buf), "%ux%u  raster %ux%u  view %.2fx  dpi %.2fx",
                                      EffW, EffH, PreviewWidth, PreviewHeight, ViewZoom, PreviewDpiScale);
                        return Buf;
                    }());

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }


    void FRmlUiEditorTool::LoadFromDisk()
    {
        FString Body;
        if (!VFS::ReadFile(Body, FStringView(VirtualPath.c_str(), VirtualPath.size())))
        {
            LOG_WARN("[RmlUiEditor] Could not read '{}'.", VirtualPath.c_str());
            CodeEditor.SetText("");
            LastSyncedText.clear();
            bBufferDirty = false;
            return;
        }

        // Skipping SetText when disk matches preserves cursor, selection, scroll and undo.
        if (Body.size() == LastSyncedText.size()
            && std::memcmp(Body.data(), LastSyncedText.data(), Body.size()) == 0)
        {
            bBufferDirty = false;
            return;
        }

        const std::string_view View(Body.c_str(), Body.size());
        CodeEditor.SetText(View);
        LastSyncedText.assign(Body.c_str(), Body.size());
        bBufferDirty = false;
    }

    void FRmlUiEditorTool::ReloadDocument(bool bAlwaysReport)
    {
        if (PreviewContext == nullptr)
        {
            return;
        }

        const std::string Body = CodeEditor.GetText();
        LastSyncedText = Body;

        if (Body.empty())
        {
            RmlUi::ClearEditorContextDocument(PreviewContext);
            return;
        }

        // A .rcss is wrapped in a specimen and a template file is shown as its own chrome.
        std::string Doc;
        if (bIsStylesheet)
        {
            Doc = BuildStylesheetSpecimen(Body);
        }
        else if (IsTemplateDocument(Body))
        {
            Doc = BuildTemplatePreview(Body);
        }
        else
        {
            Doc = Body;
        }

        const FStringView View(Doc.data(), Doc.size());
        const FStringView SourceUrl(VirtualPath.c_str(), VirtualPath.size());

        TVector<RmlUi::FRmlDiagnostic> Diagnostics;
        const bool bLoaded = RmlUi::ReplaceEditorContextDocument(PreviewContext, View, SourceUrl, &Diagnostics);

        if (!bLoaded)
        {
            LOG_WARN("[RmlUiEditor] Failed to parse buffer for '{}'.", VirtualPath.c_str());
        }

        ReportReloadDiagnostics(bLoaded, Diagnostics, bAlwaysReport);
    }

    void FRmlUiEditorTool::ReportReloadDiagnostics(bool bLoaded, const TVector<RmlUi::FRmlDiagnostic>& Diagnostics,
        bool bAlwaysReport)
    {
        int32 ErrorCount = 0;
        const FString* FirstError = nullptr;

        for (const RmlUi::FRmlDiagnostic& Diagnostic : Diagnostics)
        {
            if (!Diagnostic.bError)
            {
                continue;
            }
            ++ErrorCount;
            if (FirstError == nullptr)
            {
                FirstError = &Diagnostic.Message;
            }
        }

        const int32 WarningCount = (int32)Diagnostics.size() - ErrorCount;

        FString Headline;
        if (FirstError != nullptr)
        {
            Headline = *FirstError;
        }
        else if (!Diagnostics.empty())
        {
            Headline = Diagnostics[0].Message;
        }

        // The toast has no room for a full virtual path, and the tool's tab already says which file.
        const size_t Slash = VirtualPath.find_last_of('/');
        const FString Name = Slash == FString::npos ? VirtualPath : VirtualPath.substr(Slash + 1);

        // Keyed on the outcome, so a problem is reported once and again only when it changes or clears.
        FString Signature;
        Signature = Format("{}|{}|{}|{}", bLoaded ? 1 : 0, ErrorCount, WarningCount, Headline.c_str());

        const bool bUnchanged = (Signature == LastReloadDiagnosticSignature);
        if (bUnchanged && !bAlwaysReport)
        {
            return;
        }

        const bool bWasClean = LastReloadDiagnosticSignature.empty() || bLastReloadWasClean;
        LastReloadDiagnosticSignature = Signature;
        bLastReloadWasClean = bLoaded && Diagnostics.empty();

        if (!bLoaded)
        {
            if (Headline.empty())
            {
                ImGuiX::Notifications::NotifyError("{} failed to parse.", Name.c_str());
            }
            else
            {
                ImGuiX::Notifications::NotifyError("{} failed to parse: {}", Name.c_str(), Headline.c_str());
            }
            return;
        }

        if (Diagnostics.empty())
        {
            // Only worth saying when it is news, since a clean reload is the normal case.
            if (!bWasClean)
            {
                ImGuiX::Notifications::NotifySuccess("{} reloaded cleanly.", Name.c_str());
            }
            return;
        }

        FString Suffix;
        if (Diagnostics.size() > 1)
        {
            Suffix = Format(" (+{} more)", (int32)Diagnostics.size() - 1);
        }

        if (ErrorCount > 0)
        {
            ImGuiX::Notifications::NotifyError("{}: {}{}", Name.c_str(), Headline.c_str(), Suffix.c_str());
        }
        else
        {
            ImGuiX::Notifications::NotifyWarning("{}: {}{}", Name.c_str(), Headline.c_str(), Suffix.c_str());
        }
    }

    void FRmlUiEditorTool::EnsurePreviewTarget(uint32 Width, uint32 Height)
    {
        if (Width == 0 || Height == 0)
        {
            return;
        }

        // Only the texture is quantized, so a resize or zoom drag still reuses one allocation.
        PreviewWidth  = Width;
        PreviewHeight = Height;

        constexpr uint32 Block = 64u;
        const uint32 RTWidth  = ((Width  + Block - 1) / Block) * Block;
        const uint32 RTHeight = ((Height + Block - 1) / Block) * Block;

        if (PreviewTarget.IsValid() && PreviewRTWidth == RTWidth && PreviewRTHeight == RTHeight)
        {
            return;
        }

        if (PreviewTarget.IsValid())
        {
            RmlUi::GetRenderer()->ReleaseTargetBatch(PreviewTarget.Texture);
            RHI::Textures::Release(PreviewTarget);
        }

        PreviewTarget = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = RTWidth,
            .Height = RTHeight,
            .Format = EFormat::RGBA8_UNORM,
            .bRenderTarget = true,
            .DebugName = "RmlUiEditor.PreviewTarget",
        });
        PreviewRTWidth  = RTWidth;
        PreviewRTHeight = RTHeight;

        // The content is unchanged on a resize, and the context reflows to the new size automatically.
    }

    void FRmlUiEditorTool::TearDownPreview()
    {
        if (PreviewContext != nullptr)
        {
            RmlUi::ClearEditorContextDocument(PreviewContext);
            RmlUi::SetEditorContextTarget(PreviewContext, {}, FUIntVector2(0, 0));
            RmlUi::DestroyEditorContext(PreviewContext);
            PreviewContext = nullptr;
        }
        if (PreviewTarget.IsValid())
        {
            if (FRmlUiRenderer* Renderer = RmlUi::GetRenderer())
            {
                Renderer->ReleaseTargetBatch(PreviewTarget.Texture);
            }
            RHI::Textures::Release(PreviewTarget);
        }
        PreviewWidth = 0;
        PreviewHeight = 0;
        PreviewRTWidth = 0;
        PreviewRTHeight = 0;
    }

    void FRmlUiEditorTool::StartWatching()
    {
        if (ParentDir.empty())
        {
            return;
        }

        FFixedString DiskParentDir;
        const FStringView TargetVirtual(VirtualPath.c_str(), VirtualPath.size());
        VFS::DirectoryIterator(FStringView(ParentDir.c_str(), ParentDir.size()),
            [&](const VFS::FFileInfo& Info)
            {
                if (!DiskParentDir.empty())
                {
                    return;
                }
                if (FStringView(Info.VirtualPath.c_str(), Info.VirtualPath.size()) != TargetVirtual)
                {
                    return;
                }
                FStringView Source(Info.PathSource.c_str(), Info.PathSource.size());
                const size_t Slash = Source.find_last_of('/');
                const size_t BackSlash = Source.find_last_of('\\');
                size_t Cut = FString::npos;
                if (Slash != FStringView::npos)
                {
                    Cut = Slash;
                }
                if (BackSlash != FStringView::npos && (Cut == FStringView::npos || BackSlash > Cut))
                {
                    Cut = BackSlash;
                }
                if (Cut == FStringView::npos)
                {
                    return;
                }
                DiskParentDir.assign(Source.data(), Cut);
            });

        if (DiskParentDir.empty())
        {
            return;
        }

        const FStringView FileNameView = VFS::FileName(TargetVirtual);
        const FString FileName(FileNameView.data(), FileNameView.size());

        FileWatcher.Watch(DiskParentDir, [this, FileName](const FFileEvent& Event)
        {
            if (Event.Action != EFileAction::Modified && Event.Action != EFileAction::Added)
            {
                return;
            }

            FStringView EventPath(Event.Path.c_str(), Event.Path.size());
            if (EventPath.size() < FileName.size())
            {
                return;
            }
            const FStringView Tail = EventPath.substr(EventPath.size() - FileName.size());
            if (Tail != FStringView(FileName.c_str(), FileName.size()))
            {
                return;
            }

            bExternalChangePending.store(true, Atomic::MemoryOrderRelease);
        }, false);
    }

    // Composition designer.

    void FRmlUiEditorTool::RefreshCompositionSlots()
    {
        CompSlots.clear();
        if (PreviewContext == nullptr)
        {
            return;
        }

        TVector<RmlUi::FRmlEditorSlot> Slots;
        RmlUi::EnumerateEditorSlots(PreviewContext, Slots);

        // Re-copy the buffer for assignment parsing only when the text actually changed.
        const size_t Undo = CodeEditor.GetUndoIndex();
        if (Undo != CompAssignUndoIndex || bCompAssignDirty)
        {
            CompAssignText = CodeEditor.GetText();
            CompAssignUndoIndex = Undo;
            bCompAssignDirty = false;

            // Rebuilt here rather than per frame, so the widget keeps expansion state across non-markup edits.
            bHierarchyDirty = true;
        }

        CompSlots.reserve(Slots.size());
        for (const RmlUi::FRmlEditorSlot& Src : Slots)
        {
            FCompSlot Slot;
            Slot.Id         = Src.Id;
            Slot.Tag        = Src.Tag;
            Slot.OffsetPx   = ImVec2(Src.OffsetPx.x, Src.OffsetPx.y);
            Slot.SizePx     = ImVec2(Src.SizePx.x, Src.SizePx.y);
            Slot.Depth      = Src.Depth;
            Slot.ChildCount = Src.ChildCount;

            const std::string Id(Src.Id.c_str(), Src.Id.size());
            const std::string Assigned = ParseSlotAssignment(CompAssignText, Id);
            Slot.AssignedSrc = FString(Assigned.c_str(), Assigned.size());

            // GetAbsoluteOffset excludes the CSS transform, so add it back for the overlay to sit right.
            const std::string Tf = GetInlineStyleProp(CompAssignText, Id, "transform");
            if (!Tf.empty())
            {
                const ImVec2 TDp = ParseTranslateDp(Tf);
                const float Dpi = Math::Max(0.01f, PreviewDpiScale);
                Slot.OffsetPx.x += TDp.x * Dpi;
                Slot.OffsetPx.y += TDp.y * Dpi;
            }
            CompSlots.push_back(std::move(Slot));
        }
    }

    const FRmlUiEditorTool::FCompSlot* FRmlUiEditorTool::FindSlot(const FString& Id) const
    {
        for (const FCompSlot& Slot : CompSlots)
        {
            if (Slot.Id == Id)
            {
                return &Slot;
            }
        }
        return nullptr;
    }

    void FRmlUiEditorTool::RebuildHierarchyTree(FTreeListView& Tree)
    {
        Tree.ClearTree();

        // The body is a real node, so the document folds as one and nothing-selected is expressible.
        const FTreeNodeID Body = Tree.CreateNode(InvalidTreeNode, LE_ICON_FOLDER "  body (root)");
        {
            FTreeNodeDisplay& Display = Tree.Get<FTreeNodeDisplay>(Body);
            Display.TooltipText = "The document body. New elements land here when nothing else is selected.";

            FRmlHierarchyItem& Item = Tree.EmplaceUserData<FRmlHierarchyItem>(Body);
            Item.bIsBody = true;

            Tree.Get<FTreeNodeState>(Body).bExpanded = true;
        }

        std::vector<FSourceNode> Nodes;
        ParseSourceElements(CompAssignText, Nodes);

        // One stack indexed by depth turns the parser's flat list back into a tree.
        TVector<FTreeNodeID> ParentAtDepth;
        ParentAtDepth.push_back(Body);

        for (const FSourceNode& Node : Nodes)
        {
            if (Node.Tag == "template")
            {
                continue;   // an assignment directive, shown as its parent's badge rather than its own row
            }

            const int32 Depth = Math::Max(0, Node.Depth);
            if ((int32)ParentAtDepth.size() <= Depth)
            {
                ParentAtDepth.resize(Depth + 1, Body);
            }

            const FTreeNodeID Parent = ParentAtDepth[Depth];
            const bool bHasId = !Node.Id.empty();

            const std::string Assigned = bHasId ? ParseSlotAssignment(CompAssignText, Node.Id) : std::string();
            const bool bAssigned = !Assigned.empty();

            const char* Icon = bAssigned ? LE_ICON_PUZZLE
                             : (bHasId   ? LE_ICON_CHECKBOX_BLANK_OUTLINE
                                         : LE_ICON_SHAPE_OUTLINE);

            char Label[200];
            if (bHasId) std::snprintf(Label, sizeof(Label), "%s  #%s", Icon, Node.Id.c_str());
            else        std::snprintf(Label, sizeof(Label), "%s  <%s>", Icon, Node.Tag.c_str());

            const FTreeNodeID Handle = Tree.CreateNode(Parent, Label);

            FTreeNodeDisplay& Display = Tree.Get<FTreeNodeDisplay>(Handle);

            // Tinting the whole row is readable now that the icon is part of the text and has no own color.
            if (bAssigned)
            {
                Display.DisplayColor = ImVec4(0.45f, 0.75f, 1.0f, 1.0f);
            }

            if (bAssigned)
            {
                // A chip keeps it out of the label, so filtering still matches on tag and id alone.
                Display.TooltipChipHeader = "Widget";
                Display.TooltipChips.push_back(FString(Assigned.c_str(), Assigned.size()));
            }

            FRmlHierarchyItem& Item = Tree.EmplaceUserData<FRmlHierarchyItem>(Handle);
            Item.Tag    = Node.Tag;
            Item.Id     = Node.Id;
            Item.OpenLt = Node.OpenLt;

            Tree.Get<FTreeNodeState>(Handle).bExpanded = true;

            // Anything deeper than this node parents to it, until a sibling at this depth replaces it.
            if ((int32)ParentAtDepth.size() <= Depth + 1)
            {
                ParentAtDepth.resize(Depth + 2, Body);
            }
            ParentAtDepth[Depth + 1] = Handle;
        }

        bHierarchyDirty = false;
    }

    void FRmlUiEditorTool::DrawHierarchyPanel()
    {
        if (!HasElementTree())
        {
            ImGui::TextWrapped(LE_ICON_INFORMATION_OUTLINE
                " Stylesheets have no document tree. Open the .rml that links this sheet to author elements.");
            return;
        }

        ImGui::Checkbox("Overlays", &bShowSlotOverlays);
        ImGuiX::TextTooltip("Outline the document's id'd elements over the preview canvas.");

        ImGui::SameLine();
        ImGui::BeginDisabled(!bShowSlotOverlays);
        ImGui::SetNextItemWidth(110.0f);
        const char* kDetailLabels[] = { "All", "Assigned", "Selection" };
        int DetailIdx = (int)OverlayDetail;
        if (ImGui::Combo("##overlay_detail", &DetailIdx, kDetailLabels, IM_ARRAYSIZE(kDetailLabels)))
        {
            OverlayDetail = (EOverlayDetail)DetailIdx;
        }
        ImGuiX::TextTooltip("How much of the document to outline. Selection-only keeps a busy layout readable.");
        ImGui::EndDisabled();

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##hierarchy_search", LE_ICON_MAGNIFY " Filter elements", HierarchySearch, sizeof(HierarchySearch));
        ImGui::Separator();

        // Selection and expansion live in the widget; this only re-supplies nodes when the source moved.
        if (bHierarchyDirty)
        {
            HierarchyTree.MarkTreeDirty();
        }

        HierarchyTree.Draw(HierarchyContext);
    }

    void FRmlUiEditorTool::RevealSelectionInCode()
    {
        if (SelectedOpenLt == ~size_t(0) || SelectedOpenLt >= CompAssignText.size())
        {
            return;
        }

        int Line = 1, Col = 1;
        OffsetToLineCol(CompAssignText, SelectedOpenLt, EditorTabSize, Line, Col);

        const int Target = Math::Max(0, Math::Min(CodeEditor.GetLineCount() - 1, Line - 1));
        CodeEditor.SetCursor(Target, Math::Max(0, Col - 1));
        CodeEditor.ScrollToLine(Target, TextEditor::Scroll::alignMiddle);
    }

    void FRmlUiEditorTool::DrawInspectorPanel()
    {
        if (!HasElementTree())
        {
            ImGui::TextWrapped(LE_ICON_INFORMATION_OUTLINE " Stylesheets have no elements to inspect.");
            return;
        }

        if (SelectedOpenLt == ~size_t(0))
        {
            ImGui::TextDisabled("Select an element in the Hierarchy.");
            return;
        }

        SectionHeader(LE_ICON_COG, "Inspector");

        if (SelectedSlotId.empty())
        {
            ImGui::Text("<%s>", SelectedTag.c_str());
        }
        else
        {
            ImGui::Text("#%s  <%s>", SelectedSlotId.c_str(), SelectedTag.c_str());
        }

        int Line = 1, Col = 1;
        if (SelectedOpenLt < CompAssignText.size())
        {
            OffsetToLineCol(CompAssignText, SelectedOpenLt, EditorTabSize, Line, Col);
        }

        ImGui::TextDisabled("Line %d", Line);
        ImGui::SameLine();
        if (ImGui::SmallButton(LE_ICON_TARGET " Reveal in code"))
        {
            RevealSelectionInCode();
        }
        ImGuiX::TextTooltip("Jump the code editor to this element's open tag.");

        ImGui::Separator();

        const std::string Sel(SelectedSlotId.c_str(), SelectedSlotId.size());

        std::string Inner;
        if (!Sel.empty() && GetEditableInnerText(CompAssignText, Sel, Inner) && !Inner.empty())
        {
            ImGui::TextDisabled(LE_ICON_FORMAT_TEXT " Text");
            ImGui::TextWrapped("%s", Inner.c_str());
            ImGui::Spacing();
        }

        const FCompSlot* Slot = FindSlot(SelectedSlotId);
        if (Slot != nullptr)
        {
            const float Dpi = Math::Max(0.01f, PreviewDpiScale);

            ImGui::TextDisabled(LE_ICON_RULER " Layout");
            ImGui::Text("Position   %.0f, %.0f dp", std::round(Slot->OffsetPx.x / Dpi), std::round(Slot->OffsetPx.y / Dpi));
            ImGui::Text("Size       %.0f x %.0f dp", std::round(Slot->SizePx.x / Dpi), std::round(Slot->SizePx.y / Dpi));
            ImGui::Text("Depth      %d", Slot->Depth);
            ImGui::Text("Children   %d", Slot->ChildCount);

            if (!Slot->AssignedSrc.empty())
            {
                ImGui::Spacing();
                ImGui::TextDisabled(LE_ICON_PUZZLE " Widget");
                ImGui::TextWrapped("%s", Slot->AssignedSrc.c_str());
            }
        }
        else if (Sel.empty())
        {
            ImGui::TextDisabled("No id, so the live preview cannot report its layout.");
        }
        else
        {
            ImGui::TextDisabled("Not in the live preview yet. It appears once the document reloads.");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Read only. Edit the document in the code editor.");
    }

    void FRmlUiEditorTool::DrawSlotOverlays(const ImVec2& CanvasMin, float ScalePx)
    {
        if (!bShowSlotOverlays || CompSlots.empty() || ScalePx <= 0.0f)
        {
            return;
        }

        ImDrawList* DL = ImGui::GetWindowDrawList();
        const ImGuiIO& Io = ImGui::GetIO();
        const bool bWindowHovered = ImGui::IsWindowHovered();
        const ImVec2 CanvasMax(CanvasMin.x + PreviewWidth * ScalePx, CanvasMin.y + PreviewHeight * ScalePx);

        // The hit rect expands to a clickable minimum without distorting the visual one.
        auto Rects = [&](const FCompSlot& Slot, ImVec2& TrueMin, ImVec2& TrueMax, ImVec2& HitMin, ImVec2& HitMax)
        {
            TrueMin = ImVec2(CanvasMin.x + Slot.OffsetPx.x * ScalePx, CanvasMin.y + Slot.OffsetPx.y * ScalePx);
            TrueMax = ImVec2(TrueMin.x + Slot.SizePx.x * ScalePx, TrueMin.y + Slot.SizePx.y * ScalePx);
            const float MinHit = 16.0f;
            HitMin = TrueMin;
            HitMax = ImVec2(Math::Max(TrueMax.x, TrueMin.x + MinHit), Math::Max(TrueMax.y, TrueMin.y + MinHit));
        };

        // Hovered slot = smallest-area hit rect under the cursor, so the innermost wins.
        if (bWindowHovered)
        {
            FString NewHover;
            float Best = FLT_MAX;
            for (const FCompSlot& Slot : CompSlots)
            {
                ImVec2 TMin, TMax, HMin, HMax;
                Rects(Slot, TMin, TMax, HMin, HMax);
                if (Io.MousePos.x >= HMin.x && Io.MousePos.x <= HMax.x &&
                    Io.MousePos.y >= HMin.y && Io.MousePos.y <= HMax.y)
                {
                    const float Area = (HMax.x - HMin.x) * (HMax.y - HMin.y);
                    if (Area < Best) { Best = Area; NewHover = Slot.Id; }
                }
            }
            // Both, so the canvas keeps its same-frame response while still expiring at end of frame.
            HoveredSlotId = NewHover;
            PendingHoveredSlotId = NewHover;
        }

        // Visuals, clipped to the canvas so nothing spills onto the rest of the pane.
        DL->PushClipRect(CanvasMin, CanvasMax, true);
        for (const FCompSlot& Slot : CompSlots)
        {
            ImVec2 TMin, TMax, HMin, HMax;
            Rects(Slot, TMin, TMax, HMin, HMax);

            const bool bAssigned = !Slot.AssignedSrc.empty();
            const bool bSel = (Slot.Id == SelectedSlotId);
            const bool bHov = (Slot.Id == HoveredSlotId);
            const bool bTiny = (TMax.x - TMin.x) < 6.0f || (TMax.y - TMin.y) < 6.0f;

            // Context slots degrade to a thin unlabeled outline so a real document stays readable.
            const bool bFocus = bSel || bHov;
            if (!bFocus)
            {
                if (OverlayDetail == EOverlayDetail::SelectionOnly)
                {
                    continue;
                }
                if (OverlayDetail == EOverlayDetail::Assigned && !bAssigned)
                {
                    DL->AddRect(TMin, TMax, IM_COL32(150, 160, 175, 55), 3.0f, 0, 1.0f);
                    continue;
                }
            }

            ImU32 Line = bAssigned ? IM_COL32(55, 138, 221, 255) : IM_COL32(93, 202, 165, 255);
            const ImU32 Fill = bAssigned ? IM_COL32(55, 138, 221, 38) : IM_COL32(29, 158, 117, 26);
            if (bSel) Line = IM_COL32(250, 210, 90, 255);
            const float Thick = bSel ? 2.5f : (bHov ? 2.0f : 1.0f);

            char Label[160];
            if (bAssigned) std::snprintf(Label, sizeof(Label), "#%s : %s", Slot.Id.c_str(), Slot.AssignedSrc.c_str());
            else           std::snprintf(Label, sizeof(Label), "#%s", Slot.Id.c_str());
            const ImVec2 Ts = ImGui::CalcTextSize(Label);

            if (bTiny)
            {
                // Anchored exactly at the top-left so it reads as a placeable marker, not a misaligned box.
                const ImVec2 P = TMin;
                DL->AddRectFilled(P, ImVec2(P.x + Ts.x + 8.0f, P.y + Ts.y + 4.0f), IM_COL32(18, 18, 26, 230), 3.0f);
                DL->AddRect(P, ImVec2(P.x + Ts.x + 8.0f, P.y + Ts.y + 4.0f), Line, 3.0f, 0, Thick);
                DL->AddText(ImVec2(P.x + 4.0f, P.y + 2.0f), Line, Label);
            }
            else
            {
                DL->AddRectFilled(TMin, TMax, Fill, 3.0f);
                DL->AddRect(TMin, TMax, Line, 3.0f, 0, Thick);

                ImVec2 TagPos(TMin.x, TMin.y - Ts.y - 3.0f);
                if (TagPos.y < CanvasMin.y) TagPos = ImVec2(TMin.x + 3.0f, TMin.y + 3.0f);
                DL->AddRectFilled(TagPos, ImVec2(TagPos.x + Ts.x + 6.0f, TagPos.y + Ts.y + 3.0f), IM_COL32(18, 18, 26, 220), 2.0f);
                DL->AddText(ImVec2(TagPos.x + 3.0f, TagPos.y + 1.0f), Line, Label);
            }
        }
        DL->PopClipRect();

        // Submitted first so slot hit rects win, since otherwise a canvas selection could not be undone.
        const ImVec2 CursorRestore = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(CanvasMin);
        ImGui::InvisibleButton("##canvas_backdrop", ImVec2(CanvasMax.x - CanvasMin.x, CanvasMax.y - CanvasMin.y));
        const bool bBackdropClicked = ImGui::IsItemClicked();
        ImGui::SetCursorScreenPos(CursorRestore);

        // Submitted innermost-first so an overlapping parent cannot steal the hit.
        for (int i = (int)CompSlots.size() - 1; i >= 0; --i)
        {
            const FCompSlot& Slot = CompSlots[i];
            ImVec2 TMin, TMax, HMin, HMax;
            Rects(Slot, TMin, TMax, HMin, HMax);

            ImGui::SetCursorScreenPos(HMin);
            ImGui::PushID(i); // index, not the id string, since duplicate DOM ids from a reused widget would collide
            ImGui::InvisibleButton("##slot_hit", ImVec2(HMax.x - HMin.x, HMax.y - HMin.y));

            if (ImGui::IsItemClicked())
            {
                SelectedSlotId = Slot.Id;
                SelectedTag    = Slot.Tag;
                SelectedOpenLt = LocateSlotTag(CompAssignText, std::string(Slot.Id.c_str(), Slot.Id.size())).TagStart;
            }
            ImGui::PopID();
        }

        ImGui::SetCursorScreenPos(CursorRestore);

        // Applied after the slot pass, so a backdrop click cannot race a slot that also took it.
        if (bBackdropClicked
            || (bWindowHovered && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        {
            SelectedSlotId.clear();
            SelectedTag.clear();
            SelectedOpenLt = ~size_t(0);
        }
    }

}
