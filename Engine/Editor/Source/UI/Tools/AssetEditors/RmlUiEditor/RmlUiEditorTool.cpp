#include "RmlUiEditorTool.h"

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

namespace Lumina
{
    namespace
    {
        const char* RmlEditorWindowName    = "RmlEditor";
        const char* RmlPreviewWindowName   = "RmlPreview";
        const char* RmlHierarchyWindowName = "RmlHierarchy";
        const char* RmlInspectorWindowName = "RmlInspector";
        const char* RmlPaletteWindowName   = "RmlPalette";

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

        // A random, readable color: random hue with high saturation/value so the result stays vibrant
        // rather than muddy. Backs the "Randomize colors" button in the settings popup.
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

        // Standard 16:9 game-UI resolutions plus 4K. Index 0 = "Fit to pane".
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

        // RML/RCSS identifier rule: XID-style start, hyphens allowed after, so
        // `font-size`/`border-top-left-radius` highlight as one token.
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

        // A .rcss can't render alone; wrap the live buffer in a component specimen so the preview
        // shows its effect. SourceUrl stays the .rcss path so relative refs resolve from its folder.
        std::string BuildStylesheetSpecimen(const std::string& Rcss)
        {
            std::string Doc;
            Doc.reserve(Rcss.size() + 2400);
            Doc += "<rml><head>\n<style>\n";
            Doc += Rcss;
            Doc +=
                "\n</style>\n<style>\n"
                // Don't set a font-family here: the editor context root already carries the engine default
                // family, so the scaffold inherits a loaded font. Naming a specific family that isn't
                // registered (the bug this replaces) makes RmlUi log "No font face defined" every frame.
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

        // A <template>-root .rml is reusable chrome, not a document; LoadDocumentFromMemory
        // trips RmlUi's inline-injection handler, so detect it and let the preview adapt.
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

        // Render a template's own chrome: swap the <template ...> wrapper for
        // <rml> so the framed body (with an empty content slot) previews directly.
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

        // ---- Composition designer: buffer parsing + edit helpers (operate on the raw .rml text) ----

        // A slot element's open tag located in the source buffer, used to read assignments and to know
        // where to splice <template src> in. Quote-aware so attribute values containing '>' don't fool it.
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

        // Reads back the assignment a slot carries: the src of a <template> that is the slot's first child,
        // mirroring how AssignWidgetToSlot splices it in. Empty if the slot is unassigned.
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

        // Pull name= and content= off the <template ...> root tag of a widget file.
        void ParseTemplateAttrs(const std::string& Body, FString& OutName, FString& OutContent)
        {
            const size_t Open = Body.find("<template");
            if (Open == std::string::npos)
            {
                return;
            }
            const size_t End = Body.find('>', Open);
            if (End == std::string::npos)
            {
                return;
            }
            const std::string Tag = Body.substr(Open, End - Open);
            auto ReadAttr = [&Tag](const char* Key) -> FString
            {
                const size_t P = Tag.find(Key);
                if (P == std::string::npos)
                {
                    return {};
                }
                const size_t Eq = Tag.find('=', P);
                if (Eq == std::string::npos || Eq + 1 >= Tag.size())
                {
                    return {};
                }
                size_t V = Eq + 1;
                while (V < Tag.size() && (unsigned char)Tag[V] <= ' ') ++V;
                if (V >= Tag.size() || (Tag[V] != '"' && Tag[V] != '\'')) return {};
                const char Q = Tag[V];
                const size_t S = V + 1;
                const size_t E = Tag.find(Q, S);
                if (E == std::string::npos) return {};
                return FString(Tag.c_str() + S, E - S);
            };
            OutName    = ReadAttr("name=");
            OutContent = ReadAttr("content=");
        }

        // Byte offset -> (line, visual column). Columns are tab-expanded to match TextEditor coordinates.
        // Leading whitespace of the line containing Offset -- the indent a new sibling should match.
        std::string LineIndentAt(const std::string& Text, size_t Offset)
        {
            if (Offset > Text.size())
            {
                Offset = Text.size();
            }
            const size_t Prev = (Offset == 0) ? std::string::npos : Text.rfind('\n', Offset - 1);
            const size_t LineStart = (Prev == std::string::npos) ? 0 : Prev + 1;

            size_t i = LineStart;
            while (i < Text.size() && (Text[i] == ' ' || Text[i] == '\t'))
            {
                ++i;
            }
            return Text.substr(LineStart, i - LineStart);
        }

        // Start of the whitespace run immediately before Offset. Insertions swallow it and re-emit their
        // own, so a child lands on its own line whether or not the close tag was already broken out.
        size_t TrimWhitespaceBefore(const std::string& Text, size_t Offset)
        {
            size_t i = Math::Min(Offset, Text.size());
            while (i > 0)
            {
                const char C = Text[i - 1];
                if (C != ' ' && C != '\t' && C != '\n' && C != '\r')
                {
                    break;
                }
                --i;
            }
            return i;
        }

        void OffsetToLineCol(const std::string& Text, size_t Offset, int TabSize, int& OutLine, int& OutCol)
        {
            if (TabSize < 1) TabSize = 4;
            int Line = 0;
            size_t LineStart = 0;
            const size_t Clamp = std::min(Offset, Text.size());
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

        // Offset just past the <head> open tag, where the template <link> is inserted. npos if no head.
        size_t FindHeadInsertOffset(const std::string& Text)
        {
            const size_t Head = Text.find("<head");
            if (Head == std::string::npos)
            {
                return std::string::npos;
            }
            const size_t Gt = Text.find('>', Head);
            return (Gt == std::string::npos) ? std::string::npos : Gt + 1;
        }

        bool ContainsCI(const FString& Haystack, const char* Needle)
        {
            if (Needle == nullptr || *Needle == '\0')
            {
                return true;
            }
            std::string H(Haystack.c_str(), Haystack.size());
            std::string N(Needle);
            std::transform(H.begin(), H.end(), H.begin(), [](unsigned char C){ return (char)std::tolower(C); });
            std::transform(N.begin(), N.end(), N.begin(), [](unsigned char C){ return (char)std::tolower(C); });
            return H.find(N) != std::string::npos;
        }

        std::string TrimStr(const std::string& S)
        {
            size_t A = 0, B = S.size();
            while (A < B && (unsigned char)S[A] <= ' ') ++A;
            while (B > A && (unsigned char)S[B - 1] <= ' ') --B;
            return S.substr(A, B - A);
        }

        // Split an inline style value ("left: 4dp; top: 8dp") into ordered key/value pairs.
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

        std::string SerializeStyle(const std::vector<std::pair<std::string, std::string>>& Props)
        {
            std::string Out;
            for (const auto& KV : Props)
            {
                if (!Out.empty()) Out += " ";
                Out += KV.first + ": " + KV.second + ";";
            }
            return Out;
        }

        // #RGB / #RGBA / #RRGGBB / #RRGGBBAA -> RGBA float (defaults to opaque white on parse miss).
        ImVec4 ParseHexColor(const std::string& S)
        {
            auto Hx = [](char C) -> int
            {
                if (C >= '0' && C <= '9') return C - '0';
                if (C >= 'a' && C <= 'f') return 10 + (C - 'a');
                if (C >= 'A' && C <= 'F') return 10 + (C - 'A');
                return -1;
            };
            const size_t H = S.find('#');
            std::string D;
            for (size_t i = (H == std::string::npos ? 0 : H + 1); i < S.size(); ++i)
            {
                if (Hx(S[i]) < 0) break;
                D.push_back(S[i]);
            }
            int R = 255, G = 255, B = 255, A = 255;
            if (D.size() == 3)      { R = Hx(D[0]) * 17; G = Hx(D[1]) * 17; B = Hx(D[2]) * 17; }
            else if (D.size() == 4) { R = Hx(D[0]) * 17; G = Hx(D[1]) * 17; B = Hx(D[2]) * 17; A = Hx(D[3]) * 17; }
            else if (D.size() >= 6)
            {
                R = Hx(D[0]) * 16 + Hx(D[1]); G = Hx(D[2]) * 16 + Hx(D[3]); B = Hx(D[4]) * 16 + Hx(D[5]);
                if (D.size() >= 8) A = Hx(D[6]) * 16 + Hx(D[7]);
            }
            return ImVec4(R / 255.0f, G / 255.0f, B / 255.0f, A / 255.0f);
        }

        std::string FormatHexColor(const ImVec4& C)
        {
            auto B = [](float V) { return (int)(std::clamp(V, 0.0f, 1.0f) * 255.0f + 0.5f); };
            char Buf[16];
            if (C.w >= 0.999f) std::snprintf(Buf, sizeof(Buf), "#%02X%02X%02X", B(C.x), B(C.y), B(C.z));
            else               std::snprintf(Buf, sizeof(Buf), "#%02X%02X%02X%02X", B(C.x), B(C.y), B(C.z), B(C.w));
            return Buf;
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

        // Extract the translate offset (dp) from a `translate(Xdp, Ydp)` transform value. sscanf reads the
        // leading number of each component and ignores the unit suffix; we only ever author dp here.
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

        // [start,end) of the close tag </name> that matches an open tag, or {npos,npos}. Depth-aware over the
        // same tag name; nested same-tag opens raise depth, self-closing same-tag and other tags are ignored.
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

        // A unique id not already present in the buffer: Base, then Base1, Base2, ...
        std::string GenerateUniqueId(const std::string& Text, const char* Base)
        {
            auto Exists = [&](const std::string& Id)
            {
                return Text.find("id=\"" + Id + "\"") != std::string::npos
                    || Text.find("id='" + Id + "'") != std::string::npos;
            };
            if (!Exists(Base)) return Base;
            for (int i = 1; i < 100000; ++i)
            {
                std::string Cand = std::string(Base) + std::to_string(i);
                if (!Exists(Cand)) return Cand;
            }
            return std::string(Base) + "_new";
        }
        
        struct FElementPrimitive
        {
            const char* Category;
            const char* Label;
            const char* IdBase;
            const char* Markup;
        };
        const char* const kElementCategories[] = { "Panels", "Common", "Input" };
        const FElementPrimitive kElementPrimitives[] =
        {
            // Panels -------------------------------------------------------------------------------------
            { "Panels", "Canvas Panel",   "canvas",   "<div id=\"%s\" style=\"position: relative; width: 100%%; height: 100%%; min-height: 80dp;\"></div>" },
            { "Panels", "Horizontal Box", "hbox",     "<div id=\"%s\" style=\"display: flex; flex-direction: row; gap: 8dp; min-width: 140dp; min-height: 48dp;\"></div>" },
            { "Panels", "Vertical Box",   "vbox",     "<div id=\"%s\" style=\"display: flex; flex-direction: column; gap: 8dp; min-width: 140dp; min-height: 48dp;\"></div>" },
            { "Panels", "Overlay",        "overlay",  "<div id=\"%s\" style=\"position: relative; min-width: 120dp; min-height: 80dp;\"></div>" },
            { "Panels", "Wrap Box",       "wrap",     "<div id=\"%s\" style=\"display: flex; flex-wrap: wrap; gap: 8dp; min-width: 140dp; min-height: 60dp;\"></div>" },
            { "Panels", "Border",         "border",   "<div id=\"%s\" style=\"padding: 10dp; border: 1dp #45475a; border-radius: 6dp; min-width: 100dp; min-height: 48dp;\"></div>" },
            { "Panels", "Size Box",       "sizebox",  "<div id=\"%s\" style=\"width: 200dp; height: 120dp;\"></div>" },
            { "Panels", "Scroll Box",     "scroll",   "<div id=\"%s\" style=\"overflow-y: auto; max-height: 240dp; min-width: 120dp; min-height: 80dp;\"></div>" },
            { "Panels", "Panel",          "panel",    "<div id=\"%s\" style=\"padding: 12dp; background-color: #1e1e2e; border: 1dp #45475a; border-radius: 8dp; min-width: 140dp; min-height: 70dp;\"></div>" },
            // Common -------------------------------------------------------------------------------------
            { "Common", "Text",           "text",     "<span id=\"%s\">Text</span>" },
            { "Common", "Button",         "button",   "<button id=\"%s\" style=\"padding: 8dp 16dp; background-color: #585b70; color: #fff; border-radius: 6dp;\">Button</button>" },
            { "Common", "Image",          "image",    "<img id=\"%s\" style=\"width: 64dp; height: 64dp;\" src=\"\"/>" },
            { "Common", "Progress Bar",   "progress", "<progress id=\"%s\" value=\"0.5\" style=\"width: 200dp; height: 16dp; background-color: #313244; border-radius: 4dp;\"/>" },
            // Input --------------------------------------------------------------------------------------
            { "Input",  "Text Field",     "field",    "<input id=\"%s\" type=\"text\" style=\"width: 160dp;\"/>" },
            { "Input",  "Check Box",      "check",    "<input id=\"%s\" type=\"checkbox\"/>" },
            { "Input",  "Slider",         "slider",   "<input id=\"%s\" type=\"range\" min=\"0\" max=\"100\" value=\"50\" style=\"width: 160dp;\"/>" },
        };

        // Parse the tag that starts at '<' at Lt (quote-aware), filling an FSlotTagLoc. For walking siblings
        // that may not carry an id (LocateSlotTag is id-keyed; this is position-keyed).
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

        // Full source [OutStart, OutEnd) of an element given its open-tag loc (open tag .. matching close).
        bool ElementRange(const std::string& Text, const FSlotTagLoc& Open, size_t& OutStart, size_t& OutEnd)
        {
            if (!Open.bFound)
            {
                return false;
            }
            OutStart = Open.TagStart;
            if (Open.bSelfClosing)
            {
                OutEnd = Open.TagEnd + 1;
                return true;
            }
            const FCloseTagLoc Close = FindMatchingClose(Text, Open);
            if (Close.End == std::string::npos)
            {
                return false;
            }
            OutEnd = Close.End;
            return true;
        }

        // Backward depth-match the open tag <Name ...> for a close tag </Name> at CloseLt.
        size_t FindMatchingOpenBackward(const std::string& Text, const std::string& Name, size_t CloseLt)
        {
            auto Boundary = [&](size_t A) { return A >= Text.size() || Text[A] == '>' || Text[A] == '/' || std::isspace((unsigned char)Text[A]); };
            int Depth = 1;
            size_t Pos = CloseLt;
            while (Pos > 0)
            {
                const size_t Lt = Text.rfind('<', Pos - 1);
                if (Lt == std::string::npos) break;
                if (Lt + 1 < Text.size() && Text[Lt + 1] == '/')
                {
                    if (Text.compare(Lt + 2, Name.size(), Name) == 0 && Boundary(Lt + 2 + Name.size())) ++Depth;
                }
                else if (Text.compare(Lt + 1, Name.size(), Name) == 0 && Boundary(Lt + 1 + Name.size()))
                {
                    const FSlotTagLoc T = ParseElementAt(Text, Lt);
                    if (T.bFound && !T.bSelfClosing && --Depth == 0) return Lt;
                }
                Pos = Lt;
            }
            return std::string::npos;
        }

        // True (with OutInner) when a slot is a text leaf whose inner content can be edited inline: not
        // self-closing, no child elements, and either a text-ish tag or it already holds text.
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

        // A node in the document's authored hierarchy (parsed from the SOURCE, not the live DOM, so it shows
        // exactly what the user can edit -- every element, id or not -- and excludes injected widget internals).
        // One hierarchy row's source facts. Held as tree user data so every callback can act on the element
        // without re-parsing or indexing back into a parallel array that the tree could outlive.
        struct FRmlHierarchyItem
        {
            std::string Tag;
            std::string Id;      // "" until the element is acted on and EnsureElementId assigns one
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

        // Walk <body>'s subtree in source order, recording each element. Depth is a simple open/close counter
        // (assumes well-formed nesting, which the live preview already validates).
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
            // Two variants: the TextEditor supports only ONE multi-line comment pair, and .rml vs .rcss want
            // different ones. .rml uses HTML <!-- --> as the block style (so multi-line markup comments track
            // across lines); .rcss uses /* */. (A .rml's inline <style> /* */ comments aren't block-tracked,
            // an accepted edge.) Hex-color tokenizing is shared.
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
                // RML markup: HTML comments are the multi-line block style; the built-in tracker carries the
                // in-comment state across lines (the custom tokenizer no longer touches <!--).
                Lang.commentStart = "<!--";
                Lang.commentEnd = "-->";
            }
            Lang.hasSingleQuotedStrings = true;
            Lang.hasDoubleQuotedStrings = true;
            Lang.stringEscape = '\\';
            Lang.getIdentifier = GetRmlIdentifier;

            // Single-line <!-- --> + CSS hex color literals (anchors the swatch overlay and
            // stops `#abcdef` lexing as an identifier). Iterator only does ++/compare, step manually.
            Lang.customTokenizer = [](TextEditor::Iterator start, TextEditor::Iterator end, TextEditor::Color& color) -> TextEditor::Iterator
            {
                // HTML <!-- --> comments are handled by the built-in block-comment tracker (see commentStart),
                // so the tokenizer only needs the CSS hex-color literal here.
                // # followed by 3, 4, 6, or 8 hex digits, CSS color literal.
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

            // Common HTML/RML attribute names, colored as declarations so
            // `class=` and `id=` stand out from arbitrary identifiers.
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
        // Pull persisted preferences from the developer-settings object. (Syntax colors are read
        // straight from the CDO in ApplyEditorSettings, so they aren't mirrored into members here.)
        const CRmlUiEditorSettings* Settings = GetDefault<CRmlUiEditorSettings>();
        EditorFontScale         = Settings->FontScale;
        EditorTabSize           = std::max(1, std::min(8, Settings->TabSize));
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

        // Live-refresh when the RmlUi editor settings (palette, fonts, completion) are edited in the
        // global Settings panel, so color/appearance tweaks apply without reopening the editor.
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

        // Delayed change callback: fires once the editor is quiet, kept long enough that typing
        // a path doesn't reload mid-word. Compares last-synced text to ignore programmatic SetText.
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
            const ImVec2 EditorSize(Avail.x, std::max(32.0f, Avail.y - StatusBarHeight));

            // Ctrl+wheel over the editor adjusts font scale. Steal the wheel
            // so TextEditor doesn't also use it for vertical scroll.
            const ImVec2 EditorMin = ImGui::GetCursorScreenPos();
            const ImVec2 EditorMax(EditorMin.x + EditorSize.x, EditorMin.y + EditorSize.y);
            ImGuiIO& Io = ImGui::GetIO();
            if (Io.KeyCtrl && Io.MouseWheel != 0.0f && ImGui::IsMouseHoveringRect(EditorMin, EditorMax))
            {
                EditorFontScale = std::clamp(EditorFontScale * (1.0f + Io.MouseWheel * 0.1f), 0.5f, 4.0f);
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

        // Tag and id only, matching what the old row loop matched on. The assigned-widget name is a chip
        // rather than part of the label, so it deliberately does not widen the search.
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

            FRmlHierarchyItem& Data = Tree.Get<FRmlHierarchyItem>(Item);

            // Selecting the body means "no slot selected", which is what makes new elements land there.
            SelectedSlotId = Data.bIsBody
                ? FString()
                : (!Data.Id.empty() ? FString(Data.Id.c_str(), Data.Id.size())
                                    : EnsureElementId(Data.Tag, Data.OpenLt, Data.Id));

            // EnsureElementId rewrites the buffer, which moves every later element's offsets.
            if (!Data.bIsBody && Data.Id.empty())
            {
                bHierarchyDirty = true;
            }
        };

        HierarchyContext.HoveredFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            const FRmlHierarchyItem& Data = Tree.Get<FRmlHierarchyItem>(Item);
            if (!Data.bIsBody && !Data.Id.empty())
            {
                PendingHoveredSlotId = FString(Data.Id.c_str(), Data.Id.size());
            }
        };

        HierarchyContext.ItemContextMenuFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FRmlHierarchyItem& Data = Tree.Get<FRmlHierarchyItem>(Item);
            if (Data.bIsBody)
            {
                return;
            }

            const bool bAssigned = !Data.Id.empty() && !ParseSlotAssignment(CompAssignText, Data.Id).empty();
            DrawElementContextMenu(Data.Tag, Data.Id, Data.OpenLt, bAssigned);
        };

        HierarchyContext.DragDropFunction = [this](FTreeListView& Tree, FTreeNodeID Item)
        {
            FRmlHierarchyItem& Data = Tree.Get<FRmlHierarchyItem>(Item);
            if (Data.bIsBody)
            {
                return;
            }

            if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload("RML_WIDGET"))
            {
                AssignWidgetToSlot(EnsureElementId(Data.Tag, Data.OpenLt, Data.Id), *(const int*)Payload->Data);
                bHierarchyDirty = true;
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

        CreateToolWindow(RmlPaletteWindowName, [this](bool bFocused)
        {
            DrawPalettePanel();
        });

        RefreshWidgetLibrary();
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

        // Hover is derived state and expires with the frame that produced it. Nothing used to clear it,
        // so the last hovered slot stayed lit once the cursor left both the canvas and the tree, and a
        // lit slot draws in full whatever the overlay detail mode is, leaving no setting that could hide
        // it again. Promoted here rather than cleared at the top of the frame because the hierarchy panel
        // is registered after the preview and so publishes its hover a frame before the overlay reads it.
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

        // Hierarchy and Inspector stack so both stay visible -- selecting an element used to reflow the
        // panel because the inspector was inline. The Palette tabs behind the Hierarchy: it's used in
        // bursts when adding content, not continuously while tweaking a selection.
        ImGuiID HierarchyDockID = 0, InspectorDockID = 0;
        ImGui::DockBuilderSplitNode(DesignerDockID, ImGuiDir_Down, 0.45f, &InspectorDockID, &HierarchyDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(RmlEditorWindowName).c_str(), LeftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(RmlPreviewWindowName).c_str(), PreviewDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(RmlHierarchyWindowName).c_str(), HierarchyDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(RmlPaletteWindowName).c_str(), HierarchyDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(RmlInspectorWindowName).c_str(), InspectorDockID);
    }

    void FRmlUiEditorTool::ApplyEditorSettings()
    {
        CodeEditor.SetTabSize(std::max(1, std::min(8, EditorTabSize)));
        CodeEditor.SetInsertSpacesOnTabs(bInsertSpacesOnTabs);
        CodeEditor.SetLineSpacing(EditorLineSpacing);
        CodeEditor.SetShowWhitespacesEnabled(bEditorShowWhitespace);
        CodeEditor.SetShowLineNumbersEnabled(bEditorShowLineNumbers);
        CodeEditor.SetShowScrollbarMiniMapEnabled(bEditorShowMiniMap);
        CodeEditor.SetReadOnlyEnabled(bEditorReadOnly);
        CodeEditor.SetAutoIndentEnabled(bAutoIndent);
        CodeEditor.SetShowMatchingBrackets(bShowMatchingBrackets);
        CodeEditor.SetCompletePairedGlyphs(bCompletePairedGlyphs);

        // Start from the chosen Dark/Light base (chrome), then override the syntax-token slots with the
        // user's customizable colors from CRmlUiEditorSettings.
        TextEditor::Palette Pal = (EditorPalette == EPalette::Dark)
            ? TextEditor::GetDarkPalette()
            : TextEditor::GetLightPalette();

        const CRmlUiEditorSettings* Colors = GetDefault<CRmlUiEditorSettings>();
        auto Set = [&Pal](TextEditor::Color Slot, const FVector3& C)
        {
            const auto B = [](float V) { return (int)(std::clamp(V, 0.0f, 1.0f) * 255.0f + 0.5f); };
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

            // Fun button: roll a fresh random vibrant palette for the syntax colors. Saving fires the
            // OnSettingsSaved live-refresh, so the editor recolors immediately.
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
            const int Target = std::max(1, std::min(CodeEditor.GetLineCount(), GotoLineBuffer)) - 1;
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

            // GetText() copies the whole document; only the byte count is shown here,
            // so recompute it only when the buffer actually changed (undo index moves).
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
            if (ImGui::DragInt("##rml_w", &W, 4.0f, 16, 7680, "W %d")) CanvasWidth  = (uint32)std::max(16, W);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::DragInt("##rml_h", &H, 4.0f, 16, 4320, "H %d")) CanvasHeight = (uint32)std::max(16, H);
        }

        // Swap dimensions, handy for portrait/landscape testing without
        // re-typing W/H. Only meaningful when both dims are set.
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
                ViewZoom = std::clamp(ViewZoom * (1.0f + Io.MouseWheel * 0.1f), 0.1f, 8.0f);
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

        // The canvas ASPECT comes from the selected resolution; 0,0 means "fit to pane", which just
        // adopts the pane's own aspect so the canvas fills it with no letterboxing.
        const uint32 EffW = (CanvasWidth  != 0 && CanvasHeight != 0) ? CanvasWidth  : (uint32)std::max(16.0f, PaneSize.x);
        const uint32 EffH = (CanvasWidth  != 0 && CanvasHeight != 0) ? CanvasHeight : (uint32)std::max(16.0f, PaneSize.y);

        // Fit the canvas inside the pane at View=1.0, then scale by ViewZoom.
        const float CanvasAspect = float(EffW) / float(EffH);
        const float PaneAspect   = PaneSize.x / std::max(1.0f, PaneSize.y);
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

        // Rasterize at the size we DISPLAY at, so one context pixel is one screen pixel.
        //
        // The preview used to render at the canvas resolution and then let ImGui scale the texture into
        // this rect. ImGui samples every image bilinearly from a single mip, so any scale other than
        // exactly 1.0 resampled the whole document on the way to the pane -- and fit-to-pane never hit
        // 1.0, because the target was quantized UP to a 64px block while the rect it was drawn into was
        // the un-quantized pane. The result was a permanent ~0.75-1.0x resample that ate one-pixel font
        // stems and 1dp borders: text lost strokes and thin edges broke into dashes.
        //
        // Sizing the context to the on-screen rect removes the resample entirely. The DPI ratio then
        // carries the selected resolution's scale (see below), so layout still matches the target
        // resolution -- a 1080p preview shown in a 300px pane lays out with the same proportions, just
        // rasterized at 300px instead of rasterized at 1080 and crushed down to 300.
        const float WantW = std::max(16.0f, std::floor(FitSize.x * ViewZoom));
        const float WantH = std::max(16.0f, std::floor(FitSize.y * ViewZoom));

        // Cap the raster so zoom cannot balloon the target: at 8x on a large pane the honest size would
        // be a nine-figure pixel count. Two device pixels per screen pixel is more than the pane can
        // resolve anyway, and past the cap the image simply scales up the way it always did, which costs
        // sharpness rather than hundreds of megabytes. Applied as ONE factor so the aspect is preserved;
        // clamping the axes independently would stretch the blit.
        const float MaxRasterW = std::min(4096.0f, std::max(PaneSize.x * 2.0f, 512.0f));
        const float MaxRasterH = std::min(4096.0f, std::max(PaneSize.y * 2.0f, 512.0f));
        const float Shrink     = std::min({ 1.0f, MaxRasterW / WantW, MaxRasterH / WantH });

        const int CanvasPxW = (int)std::max(16.0f, std::floor(WantW * Shrink));
        const int CanvasPxH = (int)std::max(16.0f, std::floor(WantH * Shrink));

        EnsurePreviewTarget((uint32)CanvasPxW, (uint32)CanvasPxH);

        // Auto DPI mirrors the runtime dp convention (ratio = height / 1080). It is keyed off the
        // DISPLAYED height, which is what makes the compensation work: at the 1920x1080 preset shown in a
        // 300px-tall rect the ratio is 300/1080, so dp-sized content occupies the same fraction of the
        // canvas it would on a real 1080p screen.
        if (bAutoDpi && PreviewHeight > 0)
        {
            const float AutoDpi = std::clamp(float(PreviewHeight) / 1080.0f, 0.25f, 4.0f);
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

        // Bridge clear color: checker/transparent clears alpha=0 so ImGui composites bg
        // below the image; solid clears with the chosen color.
        FVector4 ClearColor;
        switch (BgMode)
        {
        case EBgMode::Solid:       ClearColor = FVector4(BgColor.x, BgColor.y, BgColor.z, 1.0f); break;
        case EBgMode::Checker:     // fallthrough, we draw the checker in ImGui below
        case EBgMode::Transparent: ClearColor = FVector4(0.0f, 0.0f, 0.0f, 0.0f); break;
        }
        RmlUi::SetEditorContextClearColor(PreviewContext, ClearColor);
        // The visible size, not the padded texture size: this drives both SetDimensions and the render
        // viewport, so the document lays out into exactly the region we sample back.
        RmlUi::SetEditorContextTarget(PreviewContext, PreviewTarget.Texture, FUIntVector2(PreviewWidth, PreviewHeight));

        // Unclamped, the display rect IS the raster, so the blit is exactly 1:1. Clamped (deep zoom), it
        // stays the size the user asked for and the smaller raster scales up into it.
        const ImVec2 CanvasSize = (Shrink >= 1.0f)
            ? ImVec2((float)CanvasPxW, (float)CanvasPxH)
            : ImVec2(WantW, WantH);
        const ImVec2 PaneCenter(PaneMin.x + PaneSize.x * 0.5f, PaneMin.y + PaneSize.y * 0.5f);
        // Snapped to whole pixels. Texel centers have to land on pixel centers or the 1:1 mapping above
        // is undone by a half-pixel offset, which blurs text just as effectively as scaling it.
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

        // Sample only the region the context actually rendered into. The texture is padded up to a block
        // so a resize drag reuses one allocation, and the padding is never part of the document.
        const ImTextureID Tex = (ImTextureID)(uint64)PreviewTarget.SampledSlot;
        const ImVec2 Uv1(
            float(PreviewWidth)  / float(std::max(1u, PreviewRTWidth)),
            float(PreviewHeight) / float(std::max(1u, PreviewRTHeight)));
        DL->AddImage(Tex, CanvasMin, CanvasMax, ImVec2(0.0f, 0.0f), Uv1);
        DL->AddRect(CanvasMin, CanvasMax, IM_COL32(80, 80, 95, 255), 0.0f, 0, 1.0f);

        // Convert canvas-space px to pane-space px. The context is sized to the displayed rect, so this
        // is 1.0 by construction; it stays a computed ratio so the overlay math below still holds if the
        // two ever diverge (a clamped target, a mid-resize frame).
        const float ScalePx = CanvasSize.x / float(std::max(1u, PreviewWidth));

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
                        // Both sizes, because they are now different things: the resolution the document
                        // is laid out FOR, and the pixels it is actually rasterized INTO.
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

        // Our OnSave routes back here via the watcher; if disk matches last-synced the file
        // didn't change, so skip SetText to preserve cursor/selection/scroll/undo.
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

        // .rml is a document; .rcss is wrapped in a component specimen; a
        // <template> file is shown as its own chrome (empty content slot).
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

        // Auto reload re-parses shortly after every edit, so the same broken document would toast on a
        // loop. Keyed on the outcome rather than throttled by time: the user hears about a problem the
        // moment it appears, once, and again only when it changes or clears.
        //
        // An explicit reload opts out. Saving a file and being told nothing reads as "the error went
        // away", when all that happened was that it had been reported once already and had not changed
        // since. What the user asked for is the state of the thing they just acted on.
        FString Signature;
        Signature.sprintf("%d|%d|%d|%s", bLoaded ? 1 : 0, ErrorCount, WarningCount, Headline.c_str());

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
            // Only worth saying when it is news: a clean reload is the normal case and says nothing.
            if (!bWasClean)
            {
                ImGuiX::Notifications::NotifySuccess("{} reloaded cleanly.", Name.c_str());
            }
            return;
        }

        FString Suffix;
        if (Diagnostics.size() > 1)
        {
            Suffix.sprintf(" (+%d more)", (int32)Diagnostics.size() - 1);
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

        // The visible canvas tracks the on-screen rect EXACTLY -- that is what makes one context pixel
        // one screen pixel. Only the texture is quantized, so a resize or zoom drag still reuses one
        // allocation instead of reallocating every frame.
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

        // No ReloadDocument() here: the document content is unchanged on a resize, and the context
        // reflows to the new size automatically (TickEditorContexts -> SetDimensions(E->Size)). The new
        // target is bound by the SetEditorContextTarget call right after EnsurePreviewTarget returns.
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

    //--------------------------------------------------------------------------------------------
    // Composition designer
    //--------------------------------------------------------------------------------------------

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

            // The tree is parsed from this text, so it is stale for exactly the same reason and on
            // exactly the same edits. Rebuilding here rather than per frame is what lets the widget
            // keep its expansion state across everything that is not a markup change.
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

            // GetAbsoluteOffset is the layout position and excludes the CSS transform; repositioning writes
            // an inline `transform: translate`, so add it back here for the overlay to sit on the rendered box.
            const std::string Tf = GetInlineStyleProp(CompAssignText, Id, "transform");
            if (!Tf.empty())
            {
                const ImVec2 TDp = ParseTranslateDp(Tf);
                const float Dpi = std::max(0.01f, PreviewDpiScale);
                Slot.OffsetPx.x += TDp.x * Dpi;
                Slot.OffsetPx.y += TDp.y * Dpi;
            }
            CompSlots.push_back(std::move(Slot));
        }
    }

    void FRmlUiEditorTool::RefreshWidgetLibrary()
    {
        CompWidgets.clear();
        bWidgetLibraryDirty = false;
        if (ParentDir.empty())
        {
            return;
        }

        VFS::RecursiveDirectoryIterator(FStringView(ParentDir.c_str(), ParentDir.size()),
            [&](const VFS::FFileInfo& Info)
            {
                if (Info.IsDirectory() || Info.GetExt() != ".rml")
                {
                    return;
                }

                const FString WidgetPath(Info.VirtualPath.c_str(), Info.VirtualPath.size());
                if (WidgetPath == VirtualPath)
                {
                    return; // never list the document being edited
                }

                FString Body;
                if (!VFS::ReadFile(Body, FStringView(WidgetPath.c_str(), WidgetPath.size())))
                {
                    return;
                }
                const std::string B(Body.c_str(), Body.size());
                if (!IsTemplateDocument(B))
                {
                    return; // only <template>-rooted files are reusable widgets
                }

                FCompWidget Widget;
                Widget.VirtualPath = WidgetPath;
                const FStringView NameView = VFS::FileName(FStringView(WidgetPath.c_str(), WidgetPath.size()), true);
                Widget.DisplayName = FString(NameView.data(), NameView.size());
                ParseTemplateAttrs(B, Widget.TemplateName, Widget.ContentSlotId);
                if (Widget.TemplateName.empty())
                {
                    Widget.TemplateName = Widget.DisplayName; // fall back to the file name as src
                }
                CompWidgets.push_back(std::move(Widget));
            });

        std::sort(CompWidgets.begin(), CompWidgets.end(),
            [](const FCompWidget& A, const FCompWidget& B) { return A.DisplayName < B.DisplayName; });
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

    void FRmlUiEditorTool::DrawElementContextMenu(const std::string& Tag, const std::string& Id, size_t OpenLt, bool bAssigned)
    {
        if (!ImGui::BeginPopupContextItem())
        {
            return;
        }

        if (!Id.empty()) ImGui::TextDisabled("#%s  <%s>", Id.c_str(), Tag.c_str());
        else             ImGui::TextDisabled("<%s>", Tag.c_str());
        ImGui::Separator();

        if (ImGui::BeginMenu(LE_ICON_PLUS_BOX " Add child"))
        {
            const int Count = (int)(sizeof(kElementPrimitives) / sizeof(kElementPrimitives[0]));
            for (const char* Cat : kElementCategories)
            {
                if (!ImGui::BeginMenu(Cat))
                {
                    continue;
                }
                for (int p = 0; p < Count; ++p)
                {
                    if (std::strcmp(kElementPrimitives[p].Category, Cat) != 0)
                    {
                        continue;
                    }
                    if (ImGui::MenuItem(kElementPrimitives[p].Label))
                    {
                        AddElement(EnsureElementId(Tag, OpenLt, Id), p);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        ImGui::BeginDisabled(!bAssigned);
        if (ImGui::MenuItem(LE_ICON_CLOSE " Clear widget"))
        {
            ClearSlotAssignment(FString(Id.c_str(), Id.size()));
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        if (ImGui::MenuItem(LE_ICON_ARROW_UP " Move up"))
        {
            MoveElement(EnsureElementId(Tag, OpenLt, Id), true);
        }
        if (ImGui::MenuItem(LE_ICON_ARROW_DOWN " Move down"))
        {
            MoveElement(EnsureElementId(Tag, OpenLt, Id), false);
        }

        ImGui::Separator();
        if (ImGui::MenuItem(LE_ICON_TRASH_CAN_OUTLINE " Delete element"))
        {
            RemoveElement(EnsureElementId(Tag, OpenLt, Id));
        }
        ImGui::EndPopup();
    }

    void FRmlUiEditorTool::RebuildHierarchyTree(FTreeListView& Tree)
    {
        Tree.ClearTree();

        // The body is a real node rather than a separate row above the tree, so the whole document folds
        // as one and "nothing selected" is expressible as selecting it.
        // Icons go in the label rather than FTreeNodeDisplay::IconText: the widget draws IconText over the
        // row's text instead of reserving space ahead of it, so a glyph and the name overlap.
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

        // Depth is a running counter from the parser, so the parent of a node at depth D is the most
        // recent node at depth D-1. One stack indexed by depth turns the flat list back into a tree.
        TVector<FTreeNodeID> ParentAtDepth;
        ParentAtDepth.push_back(Body);

        for (const FSourceNode& Node : Nodes)
        {
            if (Node.Tag == "template")
            {
                continue;   // an assignment directive: shown as its parent's badge, not its own row
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

            // Tints the whole row, which is the readable way to show assignment now that the icon is
            // part of the text and no longer has a colour of its own.
            if (bAssigned)
            {
                Display.DisplayColor = ImVec4(0.45f, 0.75f, 1.0f, 1.0f);
            }

            if (bAssigned)
            {
                // The inline badge the old row drew after the label. A chip keeps it out of the label so
                // filtering still matches on tag and id alone.
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
        if (!SupportsElementAuthoring())
        {
            ImGui::TextWrapped(LE_ICON_INFORMATION_OUTLINE
                " Stylesheets have no document tree. Open the .rml that links this sheet to author elements.");
            return;
        }

        if (ImGui::Button(LE_ICON_REFRESH " Rescan"))
        {
            bWidgetLibraryDirty = true;
        }
        ImGuiX::TextTooltip("Re-scan this document's folder for <template> widgets.");

        ImGui::SameLine();
        ImGui::Checkbox("Overlays", &bShowSlotOverlays);
        ImGuiX::TextTooltip("Draw slot outlines over the preview canvas. Ctrl+drag a slot to position it.");

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

        // Delete clears the selected slot's widget (ignored while typing in a field).
        if (!SelectedSlotId.empty() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
            && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Delete))
        {
            ClearSlotAssignment(SelectedSlotId);
        }
    }

    void FRmlUiEditorTool::DrawPalettePanel()
    {
        if (!SupportsElementAuthoring())
        {
            ImGui::TextWrapped(LE_ICON_INFORMATION_OUTLINE
                " Stylesheets have no elements to place. Open the .rml that links this sheet.");
            return;
        }

        if (bWidgetLibraryDirty)
        {
            RefreshWidgetLibrary();
        }

        SectionHeader(LE_ICON_PLUS_BOX, "Add element");
        if (SelectedSlotId.empty())
        {
            ImGui::TextDisabled("Into: body  (select a container to nest)");
        }
        else
        {
            ImGui::TextDisabled("Into: #%s", SelectedSlotId.c_str());
        }

        {
            const int Count = (int)(sizeof(kElementPrimitives) / sizeof(kElementPrimitives[0]));
            const float BtnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            for (const char* Cat : kElementCategories)
            {
                if (!ImGui::CollapsingHeader(Cat, ImGuiTreeNodeFlags_DefaultOpen))
                {
                    continue;
                }
                int Col = 0;
                for (int i = 0; i < Count; ++i)
                {
                    if (std::strcmp(kElementPrimitives[i].Category, Cat) != 0)
                    {
                        continue;
                    }
                    if (Col % 2 != 0)
                    {
                        ImGui::SameLine();
                    }
                    if (ImGui::Button(kElementPrimitives[i].Label, ImVec2(BtnW, 0.0f)))
                    {
                        AddElement(SelectedSlotId, i);
                    }
                    ++Col;
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        SectionHeader(LE_ICON_CARDS, "Widgets");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##widget_search", LE_ICON_MAGNIFY " Filter", WidgetSearch, sizeof(WidgetSearch));

        if (CompWidgets.empty())
        {
            ImGui::TextWrapped("No <template> widgets found beside this document.");
        }

        for (int i = 0; i < (int)CompWidgets.size(); ++i)
        {
            const FCompWidget& Widget = CompWidgets[i];
            if (!ContainsCI(Widget.DisplayName, WidgetSearch))
            {
                continue;
            }

            ImGui::PushID(i);
            char Card[160];
            std::snprintf(Card, sizeof(Card), LE_ICON_SHAPE_OUTLINE "  %s", Widget.DisplayName.c_str());
            ImGui::Selectable(Card);

            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
            {
                ImGui::SetDragDropPayload("RML_WIDGET", &i, sizeof(int));
                ImGui::Text(LE_ICON_DRAG_VARIANT " %s", Widget.DisplayName.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !SelectedSlotId.empty())
            {
                AssignWidgetToSlot(SelectedSlotId, i);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("src=\"%s\"", Widget.TemplateName.c_str());
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Drag a widget onto a slot, or select a slot then double-click a widget.");
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

        // True screen rect of a slot (exact, so overlays line up with the rendered DOM); plus a hit rect
        // expanded to a clickable minimum for empty/degenerate containers without distorting the visual.
        auto Rects = [&](const FCompSlot& Slot, ImVec2& TrueMin, ImVec2& TrueMax, ImVec2& HitMin, ImVec2& HitMax)
        {
            TrueMin = ImVec2(CanvasMin.x + Slot.OffsetPx.x * ScalePx, CanvasMin.y + Slot.OffsetPx.y * ScalePx);
            TrueMax = ImVec2(TrueMin.x + Slot.SizePx.x * ScalePx, TrueMin.y + Slot.SizePx.y * ScalePx);
            const float MinHit = 16.0f;
            HitMin = TrueMin;
            HitMax = ImVec2(std::max(TrueMax.x, TrueMin.x + MinHit), std::max(TrueMax.y, TrueMin.y + MinHit));
        };

        // Hovered slot = smallest-area hit rect under the cursor (innermost wins). Suspended mid-drag.
        if (bWindowHovered && !bDraggingSlot)
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

            // Selection and hover always draw in full; everything else is filtered by the detail mode, and
            // context slots degrade to a thin unlabelled outline so a real document stays readable.
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
                // Empty/zero-size container: a small pill anchored exactly at the slot's top-left so it
                // reads as a real, placeable marker instead of an inflated box that misaligns.
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

                if (!bAssigned)
                {
                    const char* Hint = "drop widget";
                    const ImVec2 Hs = ImGui::CalcTextSize(Hint);
                    if (Hs.x < (TMax.x - TMin.x) && Hs.y < (TMax.y - TMin.y))
                    {
                        DL->AddText(ImVec2((TMin.x + TMax.x - Hs.x) * 0.5f, (TMin.y + TMax.y - Hs.y) * 0.5f),
                                    IM_COL32(159, 225, 203, 210), Hint);
                    }
                }
            }

            // Drag ghost: where the slot will land on release (snapped to the grid when it's shown, so the
            // preview matches what CommitSlotMove will write).
            if (bDraggingSlot && Slot.Id == DraggingSlotId)
            {
                float NewX = Slot.OffsetPx.x + DragDeltaPx.x;
                float NewY = Slot.OffsetPx.y + DragDeltaPx.y;
                if (bShowGrid && GridSize > 0.0f)
                {
                    NewX = std::round(NewX / GridSize) * GridSize;
                    NewY = std::round(NewY / GridSize) * GridSize;
                }
                const ImVec2 GMin(CanvasMin.x + NewX * ScalePx, CanvasMin.y + NewY * ScalePx);
                const ImVec2 GMax(GMin.x + (TMax.x - TMin.x), GMin.y + (TMax.y - TMin.y));
                DL->AddRect(GMin, GMax, IM_COL32(250, 210, 90, 255), 3.0f, 0, 2.0f);
                DL->AddRectFilled(GMin, GMax, IM_COL32(250, 210, 90, 30), 3.0f);
            }
        }
        DL->PopClipRect();

        // Backdrop, submitted before the slots so their hit rects win any overlap: clicking canvas that
        // is not a slot clears the selection. Without it the only way to deselect was the tree's
        // "body (root)" row, which a stylesheet never draws, so a selection made on the specimen canvas
        // could not be undone at all.
        const ImVec2 CursorRestore = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(CanvasMin);
        ImGui::InvisibleButton("##canvas_backdrop", ImVec2(CanvasMax.x - CanvasMin.x, CanvasMax.y - CanvasMin.y));
        const bool bBackdropClicked = ImGui::IsItemClicked();
        ImGui::SetCursorScreenPos(CursorRestore);

        // Interaction: submit innermost-first so an overlapping parent doesn't steal the hit.
        for (int i = (int)CompSlots.size() - 1; i >= 0; --i)
        {
            const FCompSlot& Slot = CompSlots[i];
            ImVec2 TMin, TMax, HMin, HMax;
            Rects(Slot, TMin, TMax, HMin, HMax);

            ImGui::SetCursorScreenPos(HMin);
            ImGui::PushID(i); // index, not id string: duplicate DOM ids (a widget reused N times) would collide
            ImGui::InvisibleButton("##slot_hit", ImVec2(HMax.x - HMin.x, HMax.y - HMin.y));
            const bool bThis = (DraggingSlotId == Slot.Id);

            if (ImGui::IsItemClicked())
            {
                SelectedSlotId = Slot.Id;
            }

            // Ctrl+drag nudges a slot via transform:translate (no position-mode change). Plain click selects.
            if (Io.KeyCtrl && ImGui::IsItemHovered())
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
            if (ImGui::IsItemActivated() && Io.KeyCtrl)
            {
                SelectedSlotId = Slot.Id;
                DraggingSlotId = Slot.Id;
                DragDeltaPx = ImVec2(0.0f, 0.0f);
                bDraggingSlot = false;
            }
            if (ImGui::IsItemActive() && bThis && ScalePx > 0.0f)
            {
                const ImVec2 D = Io.MouseDelta;
                if (D.x != 0.0f || D.y != 0.0f) bDraggingSlot = true;
                DragDeltaPx.x += D.x / ScalePx;
                DragDeltaPx.y += D.y / ScalePx;
            }
            if (bThis && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                if (bDraggingSlot)
                {
                    CommitSlotMove(Slot.Id, DragDeltaPx);
                }
                bDraggingSlot = false;
                DraggingSlotId.clear();
                DragDeltaPx = ImVec2(0.0f, 0.0f);
            }

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload("RML_WIDGET"))
                {
                    AssignWidgetToSlot(Slot.Id, *(const int*)Payload->Data);
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
        }

        // Applied after the slot pass, so a click the backdrop reported cannot race a slot that also
        // took it. Escape is the keyboard equivalent, gated the same way the Delete binding below is so
        // it cannot swallow the key from a field or a dialog.
        if (bBackdropClicked
            || (bWindowHovered && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        {
            SelectedSlotId.clear();
        }

        // Delete clears the selected slot's widget while the canvas is in use (ignored while editing a field).
        if (!SelectedSlotId.empty() && bWindowHovered && !ImGui::IsAnyItemActive()
            && ImGui::IsKeyPressed(ImGuiKey_Delete))
        {
            ClearSlotAssignment(SelectedSlotId);
        }
    }

    void FRmlUiEditorTool::AssignWidgetToSlot(const FString& SlotId, int WidgetIndex)
    {
        if (WidgetIndex < 0 || WidgetIndex >= (int)CompWidgets.size())
        {
            return;
        }
        const FCompWidget Widget = CompWidgets[WidgetIndex]; // copy: a clear below may rescan the library
        const std::string Id(SlotId.c_str(), SlotId.size());

        std::string Text = CodeEditor.GetText();
        if (!LocateSlotTag(Text, Id).bFound)
        {
            ImGuiX::Notifications::NotifyError("Slot '#{0}' not found in the source.", SlotId.c_str());
            return;
        }

        // Replace rather than stack if the slot already holds a widget.
        if (!ParseSlotAssignment(Text, Id).empty())
        {
            ClearSlotAssignment(SlotId);
            Text = CodeEditor.GetText();
        }

        const FSlotTagLoc Loc = LocateSlotTag(Text, Id);
        if (!Loc.bFound)
        {
            return;
        }

        // <link href>: bare file name when the widget sits in the document's folder, else the absolute
        // virtual path (the RmlUi file interface resolves both).
        FString Href;
        {
            const FStringView DocDir = VFS::Parent(FStringView(VirtualPath.c_str(), VirtualPath.size()), true);
            const FStringView WgtDir = VFS::Parent(FStringView(Widget.VirtualPath.c_str(), Widget.VirtualPath.size()), true);
            if (DocDir == WgtDir)
            {
                const FStringView Name = VFS::FileName(FStringView(Widget.VirtualPath.c_str(), Widget.VirtualPath.size()));
                Href = FString(Name.data(), Name.size());
            }
            else
            {
                Href = Widget.VirtualPath;
            }
        }
        const std::string HrefStd(Href.c_str(), Href.size());
        const std::string NameStd(Widget.TemplateName.c_str(), Widget.TemplateName.size());

        struct FEdit { size_t Start; size_t End; std::string Text; };
        std::vector<FEdit> Edits;

        // 1) Ensure the template <link> in <head>.
        if (Text.find("href=\"" + HrefStd + "\"") == std::string::npos)
        {
            const size_t HeadAt = FindHeadInsertOffset(Text);
            if (HeadAt == std::string::npos)
            {
                ImGuiX::Notifications::NotifyError("Document has no <head> for the template <link>.");
                return;
            }
            Edits.push_back({ HeadAt, HeadAt, "\n    <link type=\"text/template\" href=\"" + HrefStd + "\"/>" });
        }

        // 2) Splice <template src> as the slot's first child.
        const std::string Tpl = "\n        <template src=\"" + NameStd + "\"/>";
        if (Loc.bSelfClosing)
        {
            // <div id=.../>  ->  <div id=...><template/></div>
            Edits.push_back({ Loc.TagEnd - 1, Loc.TagEnd + 1, ">" + Tpl + "\n    </" + Loc.TagName + ">" });
        }
        else
        {
            Edits.push_back({ Loc.TagEnd + 1, Loc.TagEnd + 1, Tpl });
        }

        // Apply highest-offset-first so earlier coordinates stay valid across edits.
        std::sort(Edits.begin(), Edits.end(), [](const FEdit& A, const FEdit& B) { return A.Start > B.Start; });
        const int TabSize = CodeEditor.GetTabSize();
        for (const FEdit& E : Edits)
        {
            int L0, C0, L1, C1;
            OffsetToLineCol(Text, E.Start, TabSize, L0, C0);
            OffsetToLineCol(Text, E.End,   TabSize, L1, C1);
            CodeEditor.ReplaceSectionText(L0, C0, L1, C1, E.Text);
        }

        bBufferDirty = true;
        bCompAssignDirty = true;
        ReloadDocument();
        ImGuiX::Notifications::NotifySuccess("Assigned '{0}' to #{1}.", Widget.DisplayName.c_str(), SlotId.c_str());
    }

    void FRmlUiEditorTool::ClearSlotAssignment(const FString& SlotId)
    {
        const std::string Id(SlotId.c_str(), SlotId.size());
        const std::string Text = CodeEditor.GetText();

        const FSlotTagLoc Loc = LocateSlotTag(Text, Id);
        if (!Loc.bFound || Loc.bSelfClosing)
        {
            return;
        }

        // Excise the slotted <template ...> (first child) and the whitespace before it.
        const size_t WsStart = Loc.TagEnd + 1;
        size_t i = WsStart;
        while (i < Text.size() && (unsigned char)Text[i] <= ' ')
        {
            ++i;
        }
        static const char* Tpl = "<template";
        const size_t TplLen = std::strlen(Tpl);
        if (i + TplLen > Text.size() || Text.compare(i, TplLen, Tpl) != 0)
        {
            return;
        }
        const size_t TagClose = Text.find('>', i);
        if (TagClose == std::string::npos)
        {
            return;
        }
        const size_t RemoveEnd = TagClose + 1;

        const int TabSize = CodeEditor.GetTabSize();
        int L0, C0, L1, C1;
        OffsetToLineCol(Text, WsStart,   TabSize, L0, C0);
        OffsetToLineCol(Text, RemoveEnd, TabSize, L1, C1);
        CodeEditor.ReplaceSectionText(L0, C0, L1, C1, "");

        // The <link> is intentionally left in place; an unused template registration is harmless and a
        // shared widget is usually referenced from more than one slot.
        bBufferDirty = true;
        bCompAssignDirty = true;
        ReloadDocument();
    }

    bool FRmlUiEditorTool::EnsureDocumentBody()
    {
        if (!SupportsElementAuthoring())
        {
            ImGuiX::Notifications::NotifyError("Stylesheets have no document body -- open the .rml that uses this sheet.");
            return false;
        }

        std::string Text = CodeEditor.GetText();
        if (Text.rfind("</body>") != std::string::npos)
        {
            return true;
        }

        const int TabSize = CodeEditor.GetTabSize();
        int L0, C0, L1, C1;

        // Locate the open tag the same way the rest of the file does, then look at how it terminates.
        const size_t BodyOpen = Text.find("<body");
        const size_t BodyGt   = (BodyOpen == std::string::npos) ? std::string::npos : Text.find('>', BodyOpen);
        const bool   bFound   = (BodyGt != std::string::npos);
        const bool   bSelfClosing = bFound && BodyGt > 0 && Text[BodyGt - 1] == '/';

        // A self-closed <body/> parses fine but gives nothing to insert into; widen it into a real pair.
        if (bSelfClosing)
        {
            OffsetToLineCol(Text, BodyGt - 1, TabSize, L0, C0);
            OffsetToLineCol(Text, BodyGt + 1, TabSize, L1, C1);
            CodeEditor.ReplaceSectionText(L0, C0, L1, C1, ">\n</body>");
            bBufferDirty = true;
            bCompAssignDirty = true;
            return true;
        }

        if (bFound)
        {
            // Open tag with no close: the document is malformed and blind repair could land the close in
            // the wrong place, so leave it to the text editor rather than guessing.
            ImGuiX::Notifications::NotifyError("<body> is never closed -- fix the markup, then add elements.");
            return false;
        }

        // No body at all (new or hand-trimmed file). Scaffold one inside the document root so authoring
        // works immediately instead of dead-ending on an error.
        static const char* kRoots[] = { "</rml>", "</template>" };
        size_t InsertAt = std::string::npos;
        for (const char* Root : kRoots)
        {
            const size_t At = Text.rfind(Root);
            if (At != std::string::npos)
            {
                InsertAt = At;
                break;
            }
        }
        if (InsertAt == std::string::npos)
        {
            InsertAt = Text.size();
        }

        OffsetToLineCol(Text, InsertAt, TabSize, L0, C0);
        CodeEditor.ReplaceSectionText(L0, C0, L0, C0, "<body>\n</body>\n");
        bBufferDirty = true;
        bCompAssignDirty = true;
        ImGuiX::Notifications::NotifyInfo("Added a <body> to host elements.");
        return true;
    }

    void FRmlUiEditorTool::AddElement(const FString& TargetSlotId, int PrimitiveIndex)
    {
        if (PrimitiveIndex < 0 || PrimitiveIndex >= (int)(sizeof(kElementPrimitives) / sizeof(kElementPrimitives[0])))
        {
            return;
        }
        const FElementPrimitive& Prim = kElementPrimitives[PrimitiveIndex];
        std::string Text = CodeEditor.GetText();

        const std::string NewId = GenerateUniqueId(Text, Prim.IdBase);
        char Markup[640];
        std::snprintf(Markup, sizeof(Markup), Prim.Markup, NewId.c_str());

        size_t EditStart, EditEnd;
        std::string Replacement;
        const std::string TargetId(TargetSlotId.c_str(), TargetSlotId.size());

        // Every branch below re-emits the whitespace around its insertion point rather than appending
        // inline, so a new child always lands on its own line at the right depth instead of extending
        // the parent's line. Spaces (not tabs) to match the authored content, sized to the tab setting.
        const std::string IndentUnit((size_t)Math::Max(1, CodeEditor.GetTabSize()), ' ');

        if (TargetId.empty())
        {
            // No container selected -> append to the document body, scaffolding one if this document
            // doesn't have it yet (rather than dead-ending on an error).
            if (Text.rfind("</body>") == std::string::npos)
            {
                if (!EnsureDocumentBody())
                {
                    return;
                }
                // The scaffold was a buffer edit, so re-read and re-derive the insertion point.
                Text = CodeEditor.GetText();
            }

            const size_t BodyClose = Text.rfind("</body>");
            if (BodyClose == std::string::npos)
            {
                return;
            }
            const std::string BodyIndent  = LineIndentAt(Text, BodyClose);
            const std::string ChildIndent = BodyIndent + IndentUnit;

            EditStart   = TrimWhitespaceBefore(Text, BodyClose);
            EditEnd     = BodyClose;
            Replacement = "\n" + ChildIndent + Markup + "\n" + BodyIndent;
        }
        else
        {
            const FSlotTagLoc Open = LocateSlotTag(Text, TargetId);
            if (!Open.bFound)
            {
                ImGuiX::Notifications::NotifyError("Container '#{0}' not found.", TargetSlotId.c_str());
                return;
            }
            const std::string ContainerIndent = LineIndentAt(Text, Open.TagStart);
            const std::string ChildIndent     = ContainerIndent + IndentUnit;

            if (Open.bSelfClosing)
            {
                // <div id=.../>  ->  <div id=...>\n  markup\n</div>
                EditStart   = Open.TagEnd - 1;
                EditEnd     = Open.TagEnd + 1;
                Replacement = ">\n" + ChildIndent + Markup + "\n" + ContainerIndent + "</" + Open.TagName + ">";
            }
            else
            {
                const FCloseTagLoc Close = FindMatchingClose(Text, Open);
                if (Close.Start == std::string::npos)
                {
                    ImGuiX::Notifications::NotifyError("Couldn't find the end of '#{0}'.", TargetSlotId.c_str());
                    return;
                }
                // Append as the last child. The whitespace before the close tag is absorbed and re-emitted
                // so the child gets its own line and the close tag returns to the container's indent --
                // inserting at Close.Start alone left the child glued to the parent's open tag.
                EditStart   = TrimWhitespaceBefore(Text, Close.Start);
                EditEnd     = Close.Start;
                Replacement = "\n" + ChildIndent + Markup + "\n" + ContainerIndent;
            }
        }

        const int TabSize = CodeEditor.GetTabSize();
        int L0, C0, L1, C1;
        OffsetToLineCol(Text, EditStart, TabSize, L0, C0);
        OffsetToLineCol(Text, EditEnd,   TabSize, L1, C1);
        CodeEditor.ReplaceSectionText(L0, C0, L1, C1, Replacement);

        SelectedSlotId = FString(NewId.c_str(), NewId.size());
        bBufferDirty = true;
        bCompAssignDirty = true;
        ReloadDocument();
        ImGuiX::Notifications::NotifySuccess("Added {0} (#{1}).", Prim.Label, NewId.c_str());
    }

    void FRmlUiEditorTool::RemoveElement(const FString& SlotId)
    {
        std::string Text = CodeEditor.GetText();
        const std::string Id(SlotId.c_str(), SlotId.size());
        const FSlotTagLoc Open = LocateSlotTag(Text, Id);
        if (!Open.bFound)
        {
            return;
        }

        size_t RemoveStart = Open.TagStart;
        size_t RemoveEnd;
        if (Open.bSelfClosing)
        {
            RemoveEnd = Open.TagEnd + 1;
        }
        else
        {
            const FCloseTagLoc Close = FindMatchingClose(Text, Open);
            if (Close.End == std::string::npos)
            {
                ImGuiX::Notifications::NotifyError("Couldn't find the end of '#{0}'.", SlotId.c_str());
                return;
            }
            RemoveEnd = Close.End;
        }

        // Swallow the line's leading indentation + one trailing newline so no blank line is left behind.
        while (RemoveStart > 0 && (Text[RemoveStart - 1] == ' ' || Text[RemoveStart - 1] == '\t')) --RemoveStart;
        if (RemoveEnd < Text.size() && Text[RemoveEnd] == '\n') ++RemoveEnd;

        const int TabSize = CodeEditor.GetTabSize();
        int L0, C0, L1, C1;
        OffsetToLineCol(Text, RemoveStart, TabSize, L0, C0);
        OffsetToLineCol(Text, RemoveEnd,   TabSize, L1, C1);
        CodeEditor.ReplaceSectionText(L0, C0, L1, C1, "");

        if (SelectedSlotId == SlotId) SelectedSlotId.clear();
        bBufferDirty = true;
        bCompAssignDirty = true;
        ReloadDocument();
    }

    void FRmlUiEditorTool::MoveElement(const FString& SlotId, bool bUp)
    {
        std::string Text = CodeEditor.GetText();
        const std::string Id(SlotId.c_str(), SlotId.size());
        const FSlotTagLoc Open = LocateSlotTag(Text, Id);

        size_t EStart, EEnd;
        if (!ElementRange(Text, Open, EStart, EEnd))
        {
            return;
        }
        const std::string EText = Text.substr(EStart, EEnd - EStart);
        const int TabSize = CodeEditor.GetTabSize();

        if (!bUp)
        {
            // Swap with the next sibling element (skip the whitespace between them).
            size_t P = EEnd;
            while (P < Text.size() && std::isspace((unsigned char)Text[P])) ++P;
            if (P >= Text.size() || Text[P] != '<' || (P + 1 < Text.size() && Text[P + 1] == '/'))
            {
                return; // no next sibling (end of parent)
            }
            size_t NSStart, NSEnd;
            if (!ElementRange(Text, ParseElementAt(Text, P), NSStart, NSEnd))
            {
                return;
            }
            const std::string Sep = Text.substr(EEnd, P - EEnd);
            const std::string New = Text.substr(NSStart, NSEnd - NSStart) + Sep + EText;
            int L0, C0, L1, C1;
            OffsetToLineCol(Text, EStart, TabSize, L0, C0);
            OffsetToLineCol(Text, NSEnd,  TabSize, L1, C1);
            CodeEditor.ReplaceSectionText(L0, C0, L1, C1, New);
        }
        else
        {
            // Swap with the previous sibling element (find its source range walking backwards).
            size_t P = EStart;
            while (P > 0 && std::isspace((unsigned char)Text[P - 1])) --P;
            if (P == 0 || Text[P - 1] != '>')
            {
                return; // no previous sibling element
            }
            const size_t PrevEnd = P;
            size_t PrevStart = std::string::npos;
            if (PrevEnd >= 2 && Text[PrevEnd - 2] == '/')
            {
                PrevStart = Text.rfind('<', PrevEnd - 1); // self-closing sibling
            }
            else
            {
                const size_t Lt = Text.rfind('<', PrevEnd - 1); // '<' of the '</name>'
                if (Lt == std::string::npos || Lt + 1 >= Text.size() || Text[Lt + 1] != '/')
                {
                    return;
                }
                size_t N = Lt + 2, E = N;
                while (E < PrevEnd && !std::isspace((unsigned char)Text[E]) && Text[E] != '>') ++E;
                PrevStart = FindMatchingOpenBackward(Text, Text.substr(N, E - N), Lt);
            }
            if (PrevStart == std::string::npos)
            {
                return;
            }
            const std::string Sep = Text.substr(PrevEnd, EStart - PrevEnd);
            const std::string New = EText + Sep + Text.substr(PrevStart, PrevEnd - PrevStart);
            int L0, C0, L1, C1;
            OffsetToLineCol(Text, PrevStart, TabSize, L0, C0);
            OffsetToLineCol(Text, EEnd,      TabSize, L1, C1);
            CodeEditor.ReplaceSectionText(L0, C0, L1, C1, New);
        }

        bBufferDirty = true;
        bCompAssignDirty = true;
        ReloadDocument();
    }

    void FRmlUiEditorTool::SetElementInnerText(const FString& SlotId, const std::string& NewText)
    {
        std::string Text = CodeEditor.GetText();
        const std::string Id(SlotId.c_str(), SlotId.size());
        const FSlotTagLoc Open = LocateSlotTag(Text, Id);
        if (!Open.bFound || Open.bSelfClosing)
        {
            return;
        }
        const FCloseTagLoc Close = FindMatchingClose(Text, Open);
        if (Close.Start == std::string::npos)
        {
            return;
        }
        const int TabSize = CodeEditor.GetTabSize();
        int L0, C0, L1, C1;
        OffsetToLineCol(Text, Open.TagEnd + 1, TabSize, L0, C0);
        OffsetToLineCol(Text, Close.Start,     TabSize, L1, C1);
        CodeEditor.ReplaceSectionText(L0, C0, L1, C1, NewText);

        bBufferDirty = true;
        bCompAssignDirty = true;
        ReloadDocument();
    }

    FString FRmlUiEditorTool::EnsureElementId(const std::string& Tag, size_t OpenLt, const std::string& ExistingId)
    {
        if (!ExistingId.empty())
        {
            return FString(ExistingId.c_str(), ExistingId.size());
        }
        std::string Text = CodeEditor.GetText();
        const FSlotTagLoc T = ParseElementAt(Text, OpenLt);
        if (!T.bFound)
        {
            return FString();
        }
        const std::string NewId = GenerateUniqueId(Text, Tag.empty() ? "node" : Tag.c_str());
        const size_t InsertAt = OpenLt + 1 + T.TagName.size();   // just after "<tagname"
        const std::string Attr = " id=\"" + NewId + "\"";

        const int TabSize = CodeEditor.GetTabSize();
        int L0, C0;
        OffsetToLineCol(Text, InsertAt, TabSize, L0, C0);
        CodeEditor.ReplaceSectionText(L0, C0, L0, C0, Attr);

        bBufferDirty = true;
        bCompAssignDirty = true;
        ReloadDocument();
        return FString(NewId.c_str(), NewId.size());
    }

    void FRmlUiEditorTool::SetSlotInlineStyle(const FString& SlotId, const std::vector<std::pair<std::string, std::string>>& Sets)
    {
        if (Sets.empty())
        {
            return;
        }
        const std::string Id(SlotId.c_str(), SlotId.size());
        std::string Text = CodeEditor.GetText();
        const FSlotTagLoc Loc = LocateSlotTag(Text, Id);
        if (!Loc.bFound)
        {
            return;
        }

        // Find an existing style="" / style='' inside the open tag.
        size_t ValStart = std::string::npos, ValEnd = std::string::npos;
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
                    ValStart = VS;
                    ValEnd = VE;
                    break;
                }
            }
        }

        std::vector<std::pair<std::string, std::string>> Props;
        if (ValStart != std::string::npos)
        {
            Props = ParseStyle(Text.substr(ValStart, ValEnd - ValStart));
        }
        for (const auto& Set : Sets)
        {
            if (Set.second.empty())
            {
                // Empty value = remove the property (used by "Reset position").
                for (size_t k = 0; k < Props.size(); )
                {
                    if (Props[k].first == Set.first) Props.erase(Props.begin() + k);
                    else ++k;
                }
                continue;
            }
            bool bFound = false;
            for (auto& P : Props)
            {
                if (P.first == Set.first) { P.second = Set.second; bFound = true; break; }
            }
            if (!bFound)
            {
                Props.push_back(Set);
            }
        }
        const std::string NewStyle = SerializeStyle(Props);

        // Nothing to do: no existing style attribute and everything resolved to removals.
        if (ValStart == std::string::npos && NewStyle.empty())
        {
            return;
        }

        const int TabSize = CodeEditor.GetTabSize();
        int L0, C0, L1, C1;
        if (ValStart != std::string::npos)
        {
            OffsetToLineCol(Text, ValStart, TabSize, L0, C0);
            OffsetToLineCol(Text, ValEnd,   TabSize, L1, C1);
            CodeEditor.ReplaceSectionText(L0, C0, L1, C1, NewStyle);
        }
        else
        {
            // No style attribute yet: splice one in just before the tag's closing '>' (or '/>').
            const size_t InsertAt = Loc.bSelfClosing ? (Loc.TagEnd - 1) : Loc.TagEnd;
            const std::string Attr = " style=\"" + NewStyle + "\"";
            OffsetToLineCol(Text, InsertAt, TabSize, L0, C0);
            CodeEditor.ReplaceSectionText(L0, C0, L0, C0, Attr);
        }

        bBufferDirty = true;
        bCompAssignDirty = true;
        ReloadDocument();
    }

    void FRmlUiEditorTool::CommitSlotMove(const FString& SlotId, ImVec2 DeltaPx)
    {
        const FCompSlot* Slot = FindSlot(SlotId);
        if (Slot == nullptr)
        {
            return;
        }
        // Slot.OffsetPx is the current rendered top-left (layout + existing transform); add the drag delta.
        CommitSlotVisual(SlotId, ImVec2(Slot->OffsetPx.x + DeltaPx.x, Slot->OffsetPx.y + DeltaPx.y), bShowGrid);
    }

    void FRmlUiEditorTool::CommitSlotVisual(const FString& SlotId, ImVec2 TargetVisualPx, bool bSnapToGrid)
    {
        const FCompSlot* Slot = FindSlot(SlotId);
        if (Slot == nullptr)
        {
            return;
        }

        const float Dpi = std::max(0.01f, PreviewDpiScale);

        if (bSnapToGrid && GridSize > 0.0f)
        {
            // Snap the rendered position to the canvas grid (grid units are context px, canvas-origin-aligned).
            TargetVisualPx.x = std::round(TargetVisualPx.x / GridSize) * GridSize;
            TargetVisualPx.y = std::round(TargetVisualPx.y / GridSize) * GridSize;
        }

        // Back out the layout position (rendered - existing transform), so the new translate moves the element
        // from where layout naturally puts it. Repositioning is a relative nudge: no position-mode change, so
        // the element keeps its flow/flex/anchor behavior and stays put-relative on resize.
        const ImVec2 CurTransDp = ParseTranslateDp(GetInlineStyleProp(CodeEditor.GetText(),
            std::string(SlotId.c_str(), SlotId.size()), "transform"));
        const ImVec2 LayoutPx(Slot->OffsetPx.x - CurTransDp.x * Dpi, Slot->OffsetPx.y - CurTransDp.y * Dpi);

        const float TransDpX = std::round((TargetVisualPx.x - LayoutPx.x) / Dpi);
        const float TransDpY = std::round((TargetVisualPx.y - LayoutPx.y) / Dpi);

        char Buf[64];
        std::snprintf(Buf, sizeof(Buf), "translate(%gdp, %gdp)", TransDpX, TransDpY);
        SetSlotInlineStyle(SlotId, { { "transform", Buf } });
    }

    void FRmlUiEditorTool::DrawInspectorPanel()
    {
        if (!SupportsElementAuthoring())
        {
            ImGui::TextWrapped(LE_ICON_INFORMATION_OUTLINE " Stylesheets have no elements to inspect.");
            return;
        }

        if (SelectedSlotId.empty())
        {
            ImGui::TextDisabled("Select an element in the Hierarchy.");
            return;
        }

        const FCompSlot* Slot = FindSlot(SelectedSlotId);
        if (Slot == nullptr)
        {
            // Selected in the source tree but not yet in the live DOM (just added, or inside a collapsed
            // template). Say so rather than rendering an empty panel.
            ImGui::TextDisabled("#%s", SelectedSlotId.c_str());
            ImGui::TextWrapped("Not present in the live preview yet. It will appear once the document reloads.");
            return;
        }

        SectionHeader(LE_ICON_COG, "Inspector");
        ImGui::Text("#%s  <%s>", Slot->Id.c_str(), Slot->Tag.c_str());
        ImGui::Separator();

        // Text block for text-leaf elements (Text / Button / headings / ...): inner text + font size +
        // color. Caches are owned by their widget while active and synced to the live value otherwise.
        {
            const std::string Sel(SelectedSlotId.c_str(), SelectedSlotId.size());
            std::string Inner;
            if (GetEditableInnerText(CompAssignText, Sel, Inner))
            {
                ImGui::TextDisabled(LE_ICON_FORMAT_TEXT " Text");

                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##slot_inner_text", InspText, sizeof(InspText));
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    SetElementInnerText(SelectedSlotId, InspText);
                }
                if (!ImGui::IsItemActive())
                {
                    std::snprintf(InspText, sizeof(InspText), "%s", Inner.c_str());
                }

                // Font size (font-size, dp).
                const std::string FsStr = GetInlineStyleProp(CompAssignText, Sel, "font-size");
                float CurFs = 16.0f;
                if (!FsStr.empty()) std::sscanf(FsStr.c_str(), "%f", &CurFs);
                ImGui::SetNextItemWidth(110.0f);
                ImGui::DragFloat("Size", &InspFontSize, 0.5f, 1.0f, 300.0f, "%.0f dp");
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    char Buf[32];
                    std::snprintf(Buf, sizeof(Buf), "%gdp", std::round(InspFontSize));
                    SetSlotInlineStyle(SelectedSlotId, { { "font-size", Buf } });
                }
                if (!ImGui::IsItemActive())
                {
                    InspFontSize = std::round(CurFs);
                }

                // Color (color). Synced on selection change (a ColorEdit popup leaves the item inactive, so a
                // per-frame sync would fight the picker).
                const std::string ColStr = GetInlineStyleProp(CompAssignText, Sel, "color");
                if (SelectedSlotId != InspColorSyncId)
                {
                    InspColor = ColStr.empty() ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ParseHexColor(ColStr);
                    InspColorSyncId = SelectedSlotId;
                }
                ImGui::ColorEdit4("Color", &InspColor.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    SetSlotInlineStyle(SelectedSlotId, { { "color", FormatHexColor(InspColor) } });
                }
            }
        }

        const float Dpi = std::max(0.01f, PreviewDpiScale);
        const float CurX = std::round(Slot->OffsetPx.x / Dpi);
        const float CurY = std::round(Slot->OffsetPx.y / Dpi);
        const float CurW = std::round(Slot->SizePx.x / Dpi);
        const float CurH = std::round(Slot->SizePx.y / Dpi);

        // Drag to scrub, Ctrl+click to type. Cached members are owned by DragFloat while active and snap to
        // the live DOM value otherwise (so canvas drags / reloads / reselects flow in). Commit on release.
        // X/Y nudge the RENDERED position via transform (CommitSlotVisual) -- no position:absolute, so the
        // element keeps its layout/anchor and stays responsive. Width/Height write directly.
        auto SizeField = [&](const char* Label, float* Cached, float Current, const char* Prop)
        {
            ImGui::SetNextItemWidth(110.0f);
            ImGui::DragFloat(Label, Cached, 0.5f, 0.0f, 0.0f, "%.0f dp");
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                char Buf[32];
                std::snprintf(Buf, sizeof(Buf), "%gdp", std::round(*Cached));
                SetSlotInlineStyle(SelectedSlotId, { { Prop, Buf } });
            }
            if (!ImGui::IsItemActive())
            {
                *Cached = Current;
            }
        };

        auto PosField = [&](const char* Label, float* Cached, float Current, bool bAxisX)
        {
            ImGui::SetNextItemWidth(110.0f);
            ImGui::DragFloat(Label, Cached, 0.5f, 0.0f, 0.0f, "%.0f dp");
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                const ImVec2 Target(
                    bAxisX ? std::round(*Cached) * Dpi : Slot->OffsetPx.x,
                    bAxisX ? Slot->OffsetPx.y          : std::round(*Cached) * Dpi);
                CommitSlotVisual(SelectedSlotId, Target, false);
            }
            if (!ImGui::IsItemActive())
            {
                *Cached = Current;
            }
        };

        PosField ("X",      &InspLeft,   CurX, true);
        PosField ("Y",      &InspTop,    CurY, false);
        SizeField("Width",  &InspWidth,  CurW, "width");
        SizeField("Height", &InspHeight, CurH, "height");

        ImGui::Spacing();
        if (ImGui::Button(LE_ICON_ARROW_UP " Up"))
        {
            MoveElement(SelectedSlotId, true);
        }
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_ARROW_DOWN " Down"))
        {
            MoveElement(SelectedSlotId, false);
        }
        ImGuiX::TextTooltip("Reorder this element among its siblings.");

        ImGui::Spacing();
        if (ImGui::Button(LE_ICON_BACKUP_RESTORE " Reset position"))
        {
            // Strip every inline positioning prop so the element falls back to its RCSS layout. Also clears
            // stale position:absolute+left/top that an earlier build's drag may have left behind.
            SetSlotInlineStyle(SelectedSlotId, {
                { "transform", "" }, { "position", "" },
                { "left", "" }, { "top", "" }, { "right", "" }, { "bottom", "" } });
        }
        ImGuiX::TextTooltip("Clear inline position/transform and return the element to its stylesheet layout.");

        if (!Slot->AssignedSrc.empty())
        {
            ImGui::Spacing();
            if (ImGui::Button(LE_ICON_CLOSE " Clear widget"))
            {
                ClearSlotAssignment(SelectedSlotId);
            }
            ImGuiX::TextTooltip("Remove the assigned widget (or select the slot and press Delete).");
        }

        ImGui::Spacing();
        if (ImGui::Button(LE_ICON_TRASH_CAN_OUTLINE " Delete element"))
        {
            RemoveElement(SelectedSlotId);
        }
        ImGuiX::TextTooltip("Delete this element (and its children) from the document.");

        ImGui::TextDisabled("Drag to scrub, Ctrl+click to type. Writes inline style.");
    }
}
