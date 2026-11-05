#include "CodeEditorThemes.h"

#include "CodeEditor.h"
#include "Containers/Algorithm.h"
#include "Containers/StringFormat.h"
#include "Core/Json/Json.h"
#include "FileSystem/FileInfo.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

namespace Lumina::CodeEditorThemes
{
    namespace
    {
        constexpr const char* SyntaxSection = "Syntax";
        constexpr const char* EditorSection = "Editor";
        constexpr const char* Extension = ".json";

        constexpr const char* TokenKeys[] =
        {
            "Default", "Keyword", "Type", "Function", "KnownIdentifier", "Number", "String", "Comment",
            "Punctuation", "Preprocessor", "Tag", "Attribute", "Property", "Selector", "Value", "Binding",
        };
        static_assert(std::size(TokenKeys) == (size_t)ECodeToken::Count, "Every token needs a theme key.");

        struct FEditorKey
        {
            const char*                Name;
            ImU32 FCodeEditorPalette::* Field;
        };

        constexpr FEditorKey EditorKeys[] =
        {
            { "Background",       &FCodeEditorPalette::Background },
            { "Gutter",           &FCodeEditorPalette::Gutter },
            { "LineNumber",       &FCodeEditorPalette::LineNumber },
            { "LineNumberActive", &FCodeEditorPalette::LineNumberActive },
            { "CurrentLine",      &FCodeEditorPalette::CurrentLine },
            { "Selection",        &FCodeEditorPalette::Selection },
            { "Cursor",           &FCodeEditorPalette::Cursor },
            { "Whitespace",       &FCodeEditorPalette::Whitespace },
            { "IndentGuide",      &FCodeEditorPalette::IndentGuide },
            { "BracketMatch",     &FCodeEditorPalette::BracketMatch },
            { "WordHighlight",    &FCodeEditorPalette::WordHighlight },
            { "FindMatch",        &FCodeEditorPalette::FindMatch },
            { "Error",            &FCodeEditorPalette::Error },
            { "Warning",          &FCodeEditorPalette::Warning },
            { "Info",             &FCodeEditorPalette::Info },
            { "PopupBackground",  &FCodeEditorPalette::PopupBackground },
            { "PopupBorder",      &FCodeEditorPalette::PopupBorder },
            { "PopupSelected",    &FCodeEditorPalette::PopupSelected },
            { "PopupMatch",       &FCodeEditorPalette::PopupMatch },
            { "PopupDetail",      &FCodeEditorPalette::PopupDetail },
            { "MiniMapViewport",  &FCodeEditorPalette::MiniMapViewport },
        };

        FString ColorText(ImU32 Color)
        {
            return CodeLanguageUtil::FormatHexColor(Color, ((Color >> IM_COL32_A_SHIFT) & 0xFF) != 0xFF);
        }

        void ReadColor(const FJsonValue& Section, const char* Key, FStringView Theme, ImU32& Out)
        {
            const FJsonValue* Value = Section.Find(Key);
            if (Value == nullptr)
            {
                return;
            }
            uint32 Color = 0;
            if (CodeLanguageUtil::ParseHexColor(Value->AsString(), Color))
            {
                Out = Color;
            }
            else
            {
                LOG_WARN("Code theme '{}' gives {} a value that is not a #RRGGBB or #RRGGBBAA color.", Theme, Key);
            }
        }
    }

    TVector<FString> List()
    {
        TVector<FString> Names;
        VFS::DirectoryIterator(Directory, [&Names](VFS::FFileInfo& Info)
        {
            if (!Info.IsDirectory() && Info.GetExt() == Extension)
            {
                Names.Add(Info.Name.Mid(0, Info.Name.Len() - FStringView(Extension).Len()));
            }
        });
        Algo::Sort(Names, [](const FString& A, const FString& B) { return strcmp(*A, *B) < 0; });
        return Names;
    }

    FString PathOf(FStringView Name)
    {
        return Format("{}/{}{}", Directory, Name, Extension);
    }

    bool Load(FStringView Name, FCodeEditorPalette& Palette)
    {
        FString Text;
        if (Name.IsEmpty() || !VFS::ReadFile(Text, PathOf(Name).View()))
        {
            return false;
        }

        FJsonValue Root;
        if (!FJsonValue::Parse(Text.View(), Root, nullptr, EJsonParseFlags::AllowComments) || !Root.IsObject())
        {
            LOG_WARN("Code theme '{}' is not a JSON object, so the built-in colors stay.", Name);
            return false;
        }

        if (const FJsonValue* Syntax = Root.Find(SyntaxSection); Syntax != nullptr && Syntax->IsObject())
        {
            for (int32 Token = 0; Token < (int32)ECodeToken::Count; ++Token)
            {
                ReadColor(*Syntax, TokenKeys[Token], Name, Palette.Tokens[Token]);
            }
        }
        if (const FJsonValue* Editor = Root.Find(EditorSection); Editor != nullptr && Editor->IsObject())
        {
            for (const FEditorKey& Key : EditorKeys)
            {
                ReadColor(*Editor, Key.Name, Name, Palette.*Key.Field);
            }
        }
        return true;
    }

    bool Save(FStringView Name, const FCodeEditorPalette& Palette)
    {
        FJsonValue Root = FJsonValue::MakeObject();
        FJsonValue& Syntax = Root.FindOrAdd(SyntaxSection);
        for (int32 Token = 0; Token < (int32)ECodeToken::Count; ++Token)
        {
            Syntax.Set(TokenKeys[Token], ColorText(Palette.Tokens[Token]));
        }
        FJsonValue& Editor = Root.FindOrAdd(EditorSection);
        for (const FEditorKey& Key : EditorKeys)
        {
            Editor.Set(Key.Name, ColorText(Palette.*Key.Field));
        }
        FString Text = Root.ToString(4);
        Text += "\n";
        return VFS::WriteFile(PathOf(Name).View(), Text.View());
    }
}
