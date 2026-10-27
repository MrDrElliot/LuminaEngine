#include <gtest/gtest.h>

#include <imgui.h>
#include <imgui_internal.h>
#include "Tools/UI/ImGui/ImGuiAllocator.h"
#include "Containers/Algorithm.h"
#include "Core/Math/Math.h"
#include "Paths/Paths.h"
#include "Scripting/DotNet/DotNetHost.h"
#include <chrono>
#include <thread>
#include "UI/CodeEditor/CodeEditor.h"
#include "UI/CodeEditor/Languages/CSharpLanguage.h"
#include "UI/CodeEditor/Languages/CSharpScriptLanguage.h"
#include "UI/CodeEditor/Languages/RmlLanguage.h"
#include "UI/CodeEditor/Languages/SlangLanguage.h"

using namespace Lumina;

extern "C" DLL_IMPORT void LuminaModuleSetupImGui(void* InImGui, void* InImPlot);

namespace
{
    // Runs whole ImGui frames around one editor with no renderer behind them.
    class FCodeEditorHarness
    {
    public:

        FCodeEditorHarness()
        {
            ImGuiX::InstallImGuiAllocator();
            Context = ImGui::CreateContext();
            ImGui::SetCurrentContext(Context);
            LuminaModuleSetupImGui(Context, nullptr);
            ImGuiIO& IO = ImGui::GetIO();
            IO.DisplaySize = ImVec2(1280.0f, 720.0f);
            IO.DeltaTime = 1.0f / 60.0f;
            IO.IniFilename = nullptr;
            IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        }

        ~FCodeEditorHarness()
        {
            ImGui::DestroyContext(Context);
            LuminaModuleSetupImGui(nullptr, nullptr);
        }

        FCodeEditorHarness(const FCodeEditorHarness&) = delete;
        FCodeEditorHarness& operator=(const FCodeEditorHarness&) = delete;
        FCodeEditorHarness(FCodeEditorHarness&&) = delete;
        FCodeEditorHarness& operator=(FCodeEditorHarness&&) = delete;

        void Frame()
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::Begin("Host", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            Editor.Render("Code", ImVec2(0.0f, 0.0f));
            ImGui::End();
            ImGui::Render();
            for (ImTextureData* Texture : ImGui::GetPlatformIO().Textures)
            {
                if (Texture->Status == ImTextureStatus_WantCreate || Texture->Status == ImTextureStatus_WantUpdates)
                {
                    Texture->SetTexID((ImTextureID)1);
                    Texture->SetStatus(ImTextureStatus_OK);
                }
                else if (Texture->Status == ImTextureStatus_WantDestroy)
                {
                    Texture->SetTexID(ImTextureID_Invalid);
                    Texture->SetStatus(ImTextureStatus_Destroyed);
                }
            }
        }

        void Key(ImGuiKey Key, bool bDown)
        {
            ImGui::GetIO().AddKeyEvent(Key, bDown);
            Frame();
        }

        FCodeEditor   Editor;

    private:

        ImGuiContext* Context = nullptr;
    };

    // A bar in the text marks the caret and is taken out.
    FCodePosition TakeCaret(FString& Text)
    {
        const size_t Bar = Text.find('|');
        FCodePosition Caret;
        for (size_t i = 0; i < Bar; ++i)
        {
            Caret = Text[i] == '\n' ? FCodePosition{ Caret.Line + 1, 0 } : FCodePosition{ Caret.Line, Caret.Column + 1 };
        }
        Text.erase(Bar, 1);
        return Caret;
    }

    void TypeText(FCodeEditor& Editor, FStringView Text)
    {
        for (const char C : Text)
        {
            Editor.TypeCharacter((uint8)C);
        }
    }

    TVector<FCodeCompletionItem> Complete(const FCodeLanguage& Language, FString Text)
    {
        const FCodePosition Caret = TakeCaret(Text);
        FCodeDocument Document;
        Document.SetText(Text.View());
        Document.Highlight(Language, Caret.Line);

        FCodeCompletionRequest Request;
        Request.Document = &Document;
        Request.Caret = Caret;
        Request.WordStart = { Caret.Line, CodeLanguageUtil::WordRangeAt(Document, Language, Caret).Start.Column };
        const FStringView Line = Document.GetLineView(Caret.Line);
        Request.Word = Line.substr(Request.WordStart.Column, Caret.Column - Request.WordStart.Column);
        Request.LinePrefix = Line.substr(0, Caret.Column);
        TVector<ECodeToken> Scratch;
        Scratch.resize(Math::Max(1, Caret.Column));
        Request.CaretState = Language.Tokenize(Request.LinePrefix, Document.GetStartState(Language, Caret.Line), Scratch.data());

        TVector<FCodeCompletionItem> Items;
        Language.GetCompletions(Request, Items);
        return Items;
    }

    const FCodeCompletionItem* FindItem(const TVector<FCodeCompletionItem>& Items, FStringView Label)
    {
        for (const FCodeCompletionItem& Item : Items)
        {
            if (Item.Label.View() == Label)
            {
                return &Item;
            }
        }
        return nullptr;
    }

    bool HasMessage(const TVector<FCodeDiagnostic>& Diagnostics, FStringView Fragment)
    {
        return Algo::AnyOf(Diagnostics.begin(), Diagnostics.end(), [Fragment](const FCodeDiagnostic& Diagnostic) { return Diagnostic.Message.contains(Fragment); });
    }
}

TEST(CodeDocument, InsertAndEraseAcrossLines)
{
    FCodeDocument Document;
    Document.SetText("first\r\nsecond\nthird");
    EXPECT_EQ(Document.GetLineCount(), 3);
    EXPECT_TRUE(Document.GetText() == "first\nsecond\nthird");

    const FCodePosition End = Document.Insert({ 1, 3 }, "AB\nCD\nEF");
    EXPECT_EQ(End, (FCodePosition{ 3, 2 }));
    EXPECT_TRUE(Document.GetText() == "first\nsecAB\nCD\nEFond\nthird");

    Document.Erase({ { 0, 2 }, { 3, 2 } });
    EXPECT_TRUE(Document.GetText() == "fiond\nthird");
    EXPECT_EQ(Document.OffsetToPosition(7), (FCodePosition{ 1, 1 }));
    EXPECT_EQ(Document.PositionToOffset({ 1, 1 }), 7u);
}

TEST(CodeEditor, TypedPairsCloseAndUndoAsOneStep)
{
    FCSharpLanguage Language;
    FCodeEditor Editor;
    Editor.SetLanguage(&Language);
    Editor.SetText("");

    TypeText(Editor, "Foo(");
    EXPECT_TRUE(Editor.GetText() == "Foo()");
    EXPECT_EQ(Editor.GetCursor(), (FCodePosition{ 0, 4 }));

    TypeText(Editor, ")");
    EXPECT_TRUE(Editor.GetText() == "Foo()");
    EXPECT_EQ(Editor.GetCursor(), (FCodePosition{ 0, 5 }));

    Editor.Undo();
    EXPECT_TRUE(Editor.GetText() == "");
    Editor.Redo();
    EXPECT_TRUE(Editor.GetText() == "Foo()");
}

TEST(CodeEditor, BackspaceRemovesAnEmptyPair)
{
    FCSharpLanguage Language;
    FCodeEditor Editor;
    Editor.SetLanguage(&Language);
    Editor.SetText("");
    TypeText(Editor, "[");
    EXPECT_TRUE(Editor.GetText() == "[]");
    Editor.Backspace(false);
    EXPECT_TRUE(Editor.GetText() == "");
}

TEST(CodeEditor, EnterBetweenBracesOpensAnIndentedBlock)
{
    FSlangLanguage Language;
    FCodeEditor Editor;
    Editor.SetLanguage(&Language);
    Editor.SetText("void F()\n{}");
    Editor.SetCursor({ 1, 1 }, false);
    Editor.NewLine();
    EXPECT_TRUE(Editor.GetText() == "void F()\n{\n    \n}");
    EXPECT_EQ(Editor.GetCursor(), (FCodePosition{ 2, 4 }));
}

TEST(CodeEditor, CursorsAddedBelowTypeTogether)
{
    FCodeEditor Editor;
    Editor.SetText("a\nb\nc");
    Editor.SetCursor({ 0, 1 }, false);
    Editor.AddCursorVertical(1);
    Editor.AddCursorVertical(1);
    EXPECT_EQ(Editor.GetCursorCount(), 3);
    TypeText(Editor, "!");
    EXPECT_TRUE(Editor.GetText() == "a!\nb!\nc!");
}

TEST(CodeEditor, LineCommentToggleRoundTrips)
{
    FSlangLanguage Language;
    FCodeEditor Editor;
    Editor.SetLanguage(&Language);
    Editor.SetText("float A;\n    float B;");
    Editor.Select({ { 0, 0 }, { 1, 3 } }, false);
    Editor.ToggleLineComment();
    EXPECT_TRUE(Editor.GetText() == "// float A;\n//     float B;");
    Editor.ToggleLineComment();
    EXPECT_TRUE(Editor.GetText() == "float A;\n    float B;");
}

TEST(CodeEditor, MovedLinesCarryTheCursor)
{
    FCodeEditor Editor;
    Editor.SetText("one\ntwo\nthree");
    Editor.SetCursor({ 1, 1 }, false);
    Editor.MoveLines(-1);
    EXPECT_TRUE(Editor.GetText() == "two\none\nthree");
    EXPECT_EQ(Editor.GetCursor(), (FCodePosition{ 0, 1 }));
    Editor.DuplicateLines(1);
    EXPECT_TRUE(Editor.GetText() == "two\ntwo\none\nthree");
    EXPECT_EQ(Editor.GetCursor(), (FCodePosition{ 1, 1 }));
}

TEST(CodeEditor, SnippetSelectsItsFirstPlaceholder)
{
    FCodeEditor Editor;
    Editor.SetText("");
    Editor.InsertSnippet("for (${1:i}; ${2:n})$0");
    EXPECT_TRUE(Editor.GetText() == "for (i; n)");
    EXPECT_EQ(Editor.GetSelection(), (FCodeRange{ { 0, 5 }, { 0, 6 } }));
}

TEST(RmlLanguage, TokenizesMarkupStyleBlocksAndStyleAttributes)
{
    FRmlLanguage Language(false);
    FCodeDocument Document;
    Document.SetText("<div class=\"a\" style=\"color: #fff\">\n<!-- note\n-->\n<style>\n.card { display: flex; }\n</style>\n</div>");
    Document.Highlight(Language, Document.GetLineCount() - 1);

    EXPECT_EQ(Document.GetTokenAt({ 0, 1 }), ECodeToken::Tag);
    EXPECT_EQ(Document.GetTokenAt({ 0, 5 }), ECodeToken::Attribute);
    EXPECT_EQ(Document.GetTokenAt({ 0, 12 }), ECodeToken::String);
    EXPECT_EQ(Document.GetTokenAt({ 0, 22 }), ECodeToken::Property);
    EXPECT_EQ(Document.GetTokenAt({ 0, 29 }), ECodeToken::Number);
    EXPECT_EQ(Document.GetTokenAt({ 1, 5 }), ECodeToken::Comment);
    EXPECT_EQ(Document.GetTokenAt({ 2, 0 }), ECodeToken::Comment);
    EXPECT_EQ(Document.GetTokenAt({ 4, 0 }), ECodeToken::Selector);
    EXPECT_EQ(Document.GetTokenAt({ 4, 8 }), ECodeToken::Property);
    EXPECT_EQ(Document.GetTokenAt({ 4, 17 }), ECodeToken::Value);
    EXPECT_EQ(Document.GetTokenAt({ 5, 2 }), ECodeToken::Tag);
}

TEST(RmlLanguage, CompletesEachContext)
{
    FRmlLanguage Markup(false);
    EXPECT_NE(FindItem(Complete(Markup, "<d|"), "div"), nullptr);

    const TVector<FCodeCompletionItem> Attributes = Complete(Markup, "<input t|");
    const FCodeCompletionItem* Type = FindItem(Attributes, "type");
    ASSERT_NE(Type, nullptr);
    EXPECT_TRUE(Type->InsertText == "type=\"$1\"$0");
    EXPECT_NE(FindItem(Attributes, "data-model"), nullptr);

    EXPECT_NE(FindItem(Complete(Markup, "<input type=\"|\"/>"), "checkbox"), nullptr);
    EXPECT_NE(FindItem(Complete(Markup, "<style>\n.a { dis| }\n</style>"), "display"), nullptr);
    EXPECT_NE(FindItem(Complete(Markup, "<style>\n.a { display: f| }\n</style>"), "flex"), nullptr);
    EXPECT_NE(FindItem(Complete(Markup, "<style>\n.card {}\n</style>\n<div class=\"|\"></div>"), "card"), nullptr);
    EXPECT_NE(FindItem(Complete(Markup, "<div style=\"pos|\"></div>"), "position"), nullptr);

    FRmlLanguage Stylesheet(true);
    const FCodeCompletionItem* Red = FindItem(Complete(Stylesheet, ".a { color: r| }"), "red");
    ASSERT_NE(Red, nullptr);
    EXPECT_NE(Red->Swatch, 0u);
    EXPECT_NE(FindItem(Complete(Stylesheet, ".a { width: 10| }"), "10dp"), nullptr);
    EXPECT_NE(FindItem(Complete(Stylesheet, ".a:h| { }"), "hover"), nullptr);
    EXPECT_NE(FindItem(Complete(Stylesheet, "@k|"), "keyframes"), nullptr);
}

TEST(RmlLanguage, ClosesTagsAsTheyAreTyped)
{
    FRmlLanguage Language(false);
    FCodeEditor Editor;
    Editor.SetLanguage(&Language);
    Editor.SetText("");

    TypeText(Editor, "<div>");
    EXPECT_TRUE(Editor.GetText() == "<div></div>");
    EXPECT_EQ(Editor.GetCursor(), (FCodePosition{ 0, 5 }));
    Editor.NewLine();
    EXPECT_TRUE(Editor.GetText() == "<div>\n    \n</div>");

    Editor.SetText("<panel>\n    ");
    Editor.SetCursor({ 1, 4 }, false);
    TypeText(Editor, "</");
    EXPECT_TRUE(Editor.GetText() == "<panel>\n</panel>");
}

TEST(RmlLanguage, ReportsStructureAndStyleProblems)
{
    FRmlLanguage Language(false);
    FCodeDocument Document;
    Document.SetText("<div>\n<span>\n</div>\n<style>\n.a { colr: red; width: #ggg; display: flexx; }\n</style>");
    Document.Highlight(Language, Document.GetLineCount() - 1);

    TVector<FCodeDiagnostic> Diagnostics;
    Language.GetDiagnostics(Document, Diagnostics);
    EXPECT_TRUE(HasMessage(Diagnostics, "<span> is not closed before </div>"));
    EXPECT_TRUE(HasMessage(Diagnostics, "did you mean 'color'"));
    EXPECT_TRUE(HasMessage(Diagnostics, "'#ggg' is not a color"));
    EXPECT_TRUE(HasMessage(Diagnostics, "'flexx' is not a value of display"));
}

TEST(RmlLanguage, FindsColorsAndDescribesProperties)
{
    FRmlLanguage Language(true);
    FCodeDocument Document;
    Document.SetText(".a { color: #ff0000; background-color: rgba(0, 0, 255, 128); border-color: lime; }");
    Document.Highlight(Language, 0);

    TVector<FCodeColorSpan> Spans;
    Language.GetColorSpans(Document.GetLineView(0), Document.GetTokens(0).data(), Spans);
    ASSERT_EQ(Spans.size(), 3u);
    EXPECT_EQ(Spans[0].Color, IM_COL32(255, 0, 0, 255));
    EXPECT_EQ(Spans[1].Color, IM_COL32(0, 0, 255, 128));
    EXPECT_EQ(Spans[2].Color, IM_COL32(0, 255, 0, 255));

    FCodeHover Hover;
    ASSERT_TRUE(Language.GetHover(Document, { 0, 6 }, Hover));
    EXPECT_TRUE(Hover.Title.starts_with("color"));
}

TEST(SlangLanguage, ColorsSymbolsAndFindsTheActiveParameter)
{
    FSlangLanguage Language;
    Language.SetContextSymbols({ { "Tint", "input pin" } });
    FCodeDocument Document;
    Document.SetText("float3 C = lerp(Tint, UV0, saturate(0.5));");
    Document.Highlight(Language, 0);

    EXPECT_EQ(Document.GetTokenAt({ 0, 0 }), ECodeToken::Type);
    EXPECT_EQ(Document.GetTokenAt({ 0, 11 }), ECodeToken::Function);
    EXPECT_EQ(Document.GetTokenAt({ 0, 16 }), ECodeToken::KnownIdentifier);
    EXPECT_EQ(Document.GetTokenAt({ 0, 22 }), ECodeToken::KnownIdentifier);

    FCodeSignature Signature;
    ASSERT_TRUE(Language.GetSignature(Document, { 0, 22 }, Signature));
    EXPECT_EQ(Signature.ActiveParameter, 1);
    EXPECT_EQ(Signature.Parameters.size(), 3u);

    EXPECT_NE(FindItem(Complete(Language, "float A = satu|"), "saturate"), nullptr);
}

TEST(CSharpLanguage, VerbatimStringsSpanLines)
{
    FCSharpLanguage Language;
    FCodeDocument Document;
    Document.SetText("var Path = @\"C:\\\n\"\"quoted\"\"\n\"; int X = 1;");
    Document.Highlight(Language, Document.GetLineCount() - 1);

    EXPECT_EQ(Document.GetTokenAt({ 0, 0 }), ECodeToken::Keyword);
    EXPECT_EQ(Document.GetTokenAt({ 1, 2 }), ECodeToken::String);
    EXPECT_EQ(Document.GetTokenAt({ 2, 0 }), ECodeToken::String);
    EXPECT_EQ(Document.GetTokenAt({ 2, 3 }), ECodeToken::Type);
}

TEST(CodeEditor, LinesRemovedByInputDrawInTheSameFrame)
{
    FCodeEditorHarness Harness;
    FString Text;
    for (int32 Line = 0; Line < 80; ++Line)
    {
        Text += "    line of text {\n";
    }
    Harness.Editor.SetText(Text.View());
    Harness.Editor.Focus();
    Harness.Frame();
    Harness.Frame();
    ASSERT_TRUE(Harness.Editor.IsFocused());

    Harness.Key(ImGuiMod_Ctrl, true);
    Harness.Key(ImGuiKey_A, true);
    Harness.Key(ImGuiKey_A, false);
    Harness.Key(ImGuiMod_Ctrl, false);
    Harness.Key(ImGuiKey_Delete, true);
    Harness.Key(ImGuiKey_Delete, false);
    EXPECT_EQ(Harness.Editor.GetLineCount(), 1);

    Harness.Key(ImGuiMod_Ctrl, true);
    Harness.Key(ImGuiKey_Z, true);
    Harness.Key(ImGuiKey_Z, false);
    Harness.Key(ImGuiMod_Ctrl, false);
    EXPECT_EQ(Harness.Editor.GetLineCount(), 81);
}

class FScriptLanguageTest : public ::testing::Test
{
public:

    static void SetUpTestSuite()
    {
        Paths::InitializePaths();
        DotNet::Initialize();
    }

    static void TearDownTestSuite()
    {
        if (DotNet::IsInitialized())
        {
            DotNet::Shutdown();
        }
    }
};

TEST_F(FScriptLanguageTest, AnswersFromTheScriptCompiler)
{
    if (!DotNet::IsInitialized())
    {
        GTEST_SKIP() << "The .NET host did not start in this test process.";
    }

    FCSharpScriptLanguage Language;
    Language.SetFilePath("H:/CodeEditorScratch/Probe.cs");
    FCodeDocument Document;
    Document.SetText("class Probe\n{\n    int Count;\n    void Run()\n    {\n        int Wrong = \"text\";\n        Count = 1;\n    }\n}\n");

    // The first answer waits on Roslyn building the compilation in the background.
    TVector<FCodeDiagnostic> Diagnostics;
    bool bAnswered = false;
    for (int32 Attempt = 0; Attempt < 600 && !bAnswered; ++Attempt)
    {
        Diagnostics.clear();
        bAnswered = Language.GetDiagnostics(Document, Diagnostics);
        if (!bAnswered)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    ASSERT_TRUE(bAnswered);
    EXPECT_TRUE(HasMessage(Diagnostics, "CS0029"));

    FCodeHover Hover;
    ASSERT_TRUE(Language.GetHover(Document, { 6, 9 }, Hover));
    EXPECT_TRUE(Hover.Title.contains("Probe.Count"));

    FCodeLocation Definition;
    ASSERT_TRUE(Language.FindDefinition(Document, { 6, 9 }, Definition));
    EXPECT_EQ(Definition.Position, (FCodePosition{ 2, 8 }));

    const TVector<FCodeCompletionItem> Members = Complete(Language, "class Probe\n{\n    void Run()\n    {\n        string Name = \"x\";\n        Name.|\n    }\n}\n");
    EXPECT_NE(FindItem(Members, "Length"), nullptr);
    EXPECT_NE(FindItem(Members, "Substring"), nullptr);
    EXPECT_EQ(FindItem(Members, "class"), nullptr);

    const TVector<FCodeCompletionItem> Scope = Complete(Language, "class Probe\n{\n    int Count;\n    void Run()\n    {\n        Cou|\n    }\n}\n");
    const FCodeCompletionItem* Count = FindItem(Scope, "Count");
    ASSERT_NE(Count, nullptr);
    EXPECT_EQ(Count->Kind, ECodeCompletionKind::Field);

    Document.SetText("class Probe\n{\n    void Run()\n    {\n        System.Math.Max(1, );\n    }\n}\n");
    FCodeSignature Signature;
    ASSERT_TRUE(Language.GetSignature(Document, { 4, 27 }, Signature));
    EXPECT_TRUE(Signature.Label.contains("Max("));
    EXPECT_EQ(Signature.ActiveParameter, 1);
    EXPECT_EQ(Signature.Parameters.size(), 2u);
}
