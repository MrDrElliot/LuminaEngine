#include "CSharpLanguage.h"

namespace Lumina
{
    namespace CSharpLanguageData
    {
        constexpr const char* Keywords[] =
        {
            "abstract", "as", "base", "break", "case", "catch", "checked", "class", "const", "continue", "default",
            "delegate", "do", "else", "enum", "event", "explicit", "extern", "false", "finally", "fixed", "for",
            "foreach", "goto", "if", "implicit", "in", "interface", "internal", "is", "lock", "namespace", "new",
            "null", "operator", "out", "override", "params", "private", "protected", "public", "readonly", "ref",
            "return", "sealed", "sizeof", "stackalloc", "static", "struct", "switch", "this", "throw", "true", "try",
            "typeof", "unchecked", "unsafe", "using", "virtual", "volatile", "while",
            "add", "and", "async", "await", "by", "descending", "equals", "file", "from", "get", "global", "group",
            "init", "into", "join", "let", "managed", "nameof", "not", "notnull", "on", "or", "orderby", "partial",
            "record", "remove", "required", "scoped", "select", "set", "unmanaged", "value", "var", "when", "where",
            "with", "yield",
        };

        constexpr const char* Types[] =
        {
            "bool", "byte", "sbyte", "char", "decimal", "double", "float", "int", "uint", "nint", "nuint", "long",
            "ulong", "short", "ushort", "object", "string", "void", "dynamic",
            "List", "Dictionary", "HashSet", "IEnumerable", "Span", "ReadOnlySpan", "Action", "Func", "Task",
            "Vector2", "Vector3", "Vector4", "Quaternion", "Color", "Entity", "Transform",
        };

        constexpr FCodeSymbol Functions[] =
        {
            { "nameof", "nameof(symbol)", "The name of a symbol as a string, checked at compile time." },
            { "typeof", "typeof(Type)", "The System.Type of a type." },
            { "sizeof", "sizeof(Type)", "Size in bytes of an unmanaged type." },
            { "default", "default(Type)", "The default value of a type." },
        };
    }

    FCSharpLanguage::FCSharpLanguage()
    {
        bPreprocessor = true;
        bVerbatimStrings = true;
        AddKeywords(CSharpLanguageData::Keywords);
        AddTypes(CSharpLanguageData::Types);
        AddFunctions(CSharpLanguageData::Functions);
        AddSnippet("class", "public class ${1:Name}\n{\n\t$0\n}", "class declaration");
        AddSnippet("struct", "public struct ${1:Name}\n{\n\t$0\n}", "struct declaration");
        AddSnippet("method", "public void ${1:Method}($2)\n{\n\t$0\n}", "method");
        AddSnippet("prop", "public ${1:int} ${2:Name} { get; set; }$0", "auto property");
        AddSnippet("if", "if (${1:condition})\n{\n\t$0\n}", "if block");
        AddSnippet("else", "else\n{\n\t$0\n}", "else block");
        AddSnippet("for", "for (int i = 0; i < ${1:Count}; ++i)\n{\n\t$0\n}", "for loop");
        AddSnippet("foreach", "foreach (var ${1:item} in ${2:items})\n{\n\t$0\n}", "foreach loop");
        AddSnippet("while", "while (${1:condition})\n{\n\t$0\n}", "while loop");
        AddSnippet("switch", "switch (${1:value})\n{\n\tcase ${2:0}:\n\t\t$0\n\t\tbreak;\n}", "switch statement");
        AddSnippet("try", "try\n{\n\t$0\n}\ncatch (${1:Exception} e)\n{\n}", "try and catch");
    }
}
