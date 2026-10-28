#include "CodeEditorThemes.h"

#include <nlohmann/json.hpp>
#include "CodeEditor.h"
#include "Containers/Algorithm.h"
#include "Containers/StringFormat.h"
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

        void ReadColor(const nlohmann::json& Section, const char* Key, FStringView Theme, ImU32& Out)
        {
            const auto It = Section.find(Key);
            if (It == Section.end())
            {
                return;
            }
            uint32 Color = 0;
            const std::string Text = It->is_string() ? It->get<std::string>() : std::string();
            if (CodeLanguageUtil::ParseHexColor(FStringView(Text.data(), Text.size()), Color))
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
                Names.push_back(Info.Name.substr(0, Info.Name.size() - FStringView(Extension).size()));
            }
        });
        Algo::Sort(Names, [](const FString& A, const FString& B) { return strcmp(A.c_str(), B.c_str()) < 0; });
        return Names;
    }

    FString PathOf(FStringView Name)
    {
        return Format("{}/{}{}", Directory, Name, Extension);
    }

    bool Load(FStringView Name, FCodeEditorPalette& Palette)
    {
        FString Text;
        if (Name.empty() || !VFS::ReadFile(Text, PathOf(Name).View()))
        {
            return false;
        }

        const nlohmann::json Root = nlohmann::json::parse(Text.c_str(), Text.c_str() + Text.size(), nullptr, false, true);
        if (!Root.is_object())
        {
            LOG_WARN("Code theme '{}' is not a JSON object, so the built-in colors stay.", Name);
            return false;
        }

        if (const auto Syntax = Root.find(SyntaxSection); Syntax != Root.end() && Syntax->is_object())
        {
            for (int32 Token = 0; Token < (int32)ECodeToken::Count; ++Token)
            {
                ReadColor(*Syntax, TokenKeys[Token], Name, Palette.Tokens[Token]);
            }
        }
        if (const auto Editor = Root.find(EditorSection); Editor != Root.end() && Editor->is_object())
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
        nlohmann::ordered_json Root;
        nlohmann::ordered_json& Syntax = Root[SyntaxSection];
        for (int32 Token = 0; Token < (int32)ECodeToken::Count; ++Token)
        {
            Syntax[TokenKeys[Token]] = ColorText(Palette.Tokens[Token]).c_str();
        }
        nlohmann::ordered_json& Editor = Root[EditorSection];
        for (const FEditorKey& Key : EditorKeys)
        {
            Editor[Key.Name] = ColorText(Palette.*Key.Field).c_str();
        }
        const std::string Text = Root.dump(4) + "\n";
        return VFS::WriteFile(PathOf(Name).View(), FStringView(Text.data(), Text.size()));
    }
}
