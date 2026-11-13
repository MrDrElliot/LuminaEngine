using System.Text;
using System.Text.RegularExpressions;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>A file-scope name and the sources that define it.</summary>
public sealed class UnitySymbolConflict
{
    public required string Name { get; init; }

    public required List<string> Sources { get; init; }
}

/// <summary>
/// Finds names that are private to a translation unit but stop being private once unity merges two
/// sources into one, which is how a file-scope static or an anonymous namespace member collides.
/// </summary>
public static class UnitySymbolScanner
{
    /// <summary>Name introduced by a declaration, taken from the last identifier before its body or initializer.</summary>
    private static readonly Regex DeclaredName = new(
        @"([A-Za-z_]\w*)",
        RegexOptions.Compiled);

    private static readonly HashSet<string> Keywords = new(StringComparer.Ordinal)
    {
        "if", "for", "while", "switch", "return", "catch", "else", "do", "sizeof", "alignof",
        "new", "delete", "throw", "case", "using", "namespace", "struct", "class", "enum", "union",
        "extern", "static", "const", "constexpr", "inline", "template", "typename", "operator",
        "public", "private", "protected", "friend", "explicit", "virtual", "override", "final",
        "static_assert", "alignas", "decltype", "noexcept", "typedef", "void", "auto",
    };

    /// <summary>Names one source defines that another source in the same blob could collide with.</summary>
    public static HashSet<string> Scan(string SourcePath)
    {
        HashSet<string> Names = new(StringComparer.Ordinal);

        string Text;

        try
        {
            Text = File.ReadAllText(SourcePath);
        }
        catch (IOException Ex)
        {
            Log.Verbose("Could not read '{0}': {1}", SourcePath, Ex.Message);
            return Names;
        }

        CollectScope(Strip(Text), Names, bFileScope: true, Scope: string.Empty);
        return Names;
    }

    /// <summary>Groups the names defined by more than one of the given sources.</summary>
    public static List<UnitySymbolConflict> FindConflicts(IReadOnlyList<string> SourcePaths)
    {
        Dictionary<string, List<string>> Owners = new(StringComparer.Ordinal);

        foreach (string SourcePath in SourcePaths)
        {
            foreach (string Name in Scan(SourcePath))
            {
                if (!Owners.TryGetValue(Name, out List<string>? Sources))
                {
                    Sources = new List<string>();
                    Owners[Name] = Sources;
                }

                Sources.Add(SourcePath);
            }
        }

        return Owners
            .Where(Pair => Pair.Value.Count > 1)
            .Select(Pair => new UnitySymbolConflict { Name = Pair.Key, Sources = Pair.Value })
            .OrderByDescending(Conflict => Conflict.Sources.Count)
            .ThenBy(Conflict => Conflict.Name, StringComparer.Ordinal)
            .ToList();
    }

    /// <summary>
    /// Walks one brace scope, collecting only what it declares directly. A declaration nested inside a
    /// function body is a local and cannot collide, so descending into bodies would report noise.
    /// </summary>
    private static void CollectScope(string Text, HashSet<string> Names, bool bFileScope, string Scope)
    {
        int StatementStart = 0;

        for (int Index = 0; Index < Text.Length; Index++)
        {
            char Current = Text[Index];

            if (Current == '{')
            {
                string Header = Text.Substring(StatementStart, Index - StatementStart).Trim();
                int BodyStart = Index + 1;
                int BodyEnd = FindScopeEnd(Text, BodyStart);

                NamespaceKind Kind = ClassifyNamespace(Header, out string NamespaceLabel);

                // A namespace is a scope to walk into; a class or function body holds members and locals that cannot collide.
                if (Kind != NamespaceKind.None)
                {
                    CollectScope(
                        Text.Substring(BodyStart, BodyEnd - BodyStart),
                        Names,
                        bFileScope && Kind == NamespaceKind.Named,
                        Kind == NamespaceKind.Named ? Scope + NamespaceLabel + "::" : Scope);
                }
                else
                {
                    Record(Header, Names, bFileScope, Scope);
                }

                Index = BodyEnd;
                StatementStart = Index + 1;
                continue;
            }

            if (Current == ';')
            {
                Record(Text.Substring(StatementStart, Index - StatementStart).Trim(), Names, bFileScope, Scope);
                StatementStart = Index + 1;
            }
        }
    }

    private enum NamespaceKind
    {
        None,
        Named,
        Anonymous,
    }

    /// <summary>Classifies a scope header, handing back the namespace name when it has one.</summary>
    private static NamespaceKind ClassifyNamespace(string Header, out string Name)
    {
        Name = string.Empty;

        string Trimmed = Header.TrimEnd();
        Match Found = Regex.Match(Trimmed, @"\bnamespace\s+([A-Za-z_][\w:]*)$");

        if (Found.Success)
        {
            Name = Found.Groups[1].Value;
            return NamespaceKind.Named;
        }

        return Regex.IsMatch(Trimmed, @"\bnamespace$") ? NamespaceKind.Anonymous : NamespaceKind.None;
    }

    /// <summary>At file scope only 'static' has internal linkage; inside an anonymous namespace everything does.</summary>
    private static void Record(string Declaration, HashSet<string> Names, bool bFileScope, string Scope)
    {
        if (Declaration.Length == 0 || Declaration.StartsWith("#", StringComparison.Ordinal))
        {
            return;
        }

        if (bFileScope && !Regex.IsMatch(Declaration, @"(^|\s)static(\s|$)"))
        {
            return;
        }

        // Everything after an initializer or a parameter list is a value, not the name being declared.
        int Cut = Declaration.IndexOf('=');
        string Head = Cut >= 0 ? Declaration.Substring(0, Cut) : Declaration;

        Cut = Head.IndexOf('(');

        if (Cut >= 0)
        {
            Head = Head.Substring(0, Cut);
        }

        // An array bound holds identifiers of its own, and the declared name always precedes it.
        Head = Regex.Replace(Head, @"\[[^\]]*\]", " ");

        // A base clause or an enum's underlying type follows a single colon, and the name precedes it.
        Match BaseClause = Regex.Match(Head, @"(?<!:):(?!:)");

        if (BaseClause.Success)
        {
            Head = Head.Substring(0, BaseClause.Index);
        }

        MatchCollection Identifiers = DeclaredName.Matches(Head);

        if (Identifiers.Count == 0)
        {
            return;
        }

        string Name = Identifiers[^1].Groups[1].Value;

        if (!Keywords.Contains(Name))
        {
            Names.Add(Scope + Name);
        }
    }

    private static int FindScopeEnd(string Text, int BodyStart)
    {
        int Depth = 1;
        int Index = BodyStart;

        while (Depth > 0 && Index < Text.Length)
        {
            if (Text[Index] == '{')
            {
                Depth++;
            }
            else if (Text[Index] == '}')
            {
                Depth--;
            }

            Index++;
        }

        return Index - 1;
    }

    /// <summary>Removes comments, literals and preprocessor lines, so brace counting cannot be fooled.</summary>
    private static string Strip(string Text)
    {
        StringBuilder Output = new(Text.Length);

        for (int Index = 0; Index < Text.Length; Index++)
        {
            char Current = Text[Index];
            char Next = Index + 1 < Text.Length ? Text[Index + 1] : '\0';

            if (Current == '/' && Next == '/')
            {
                while (Index < Text.Length && Text[Index] != '\n')
                {
                    Index++;
                }

                Output.Append('\n');
                continue;
            }

            if (Current == '/' && Next == '*')
            {
                Index += 2;

                while (Index + 1 < Text.Length && !(Text[Index] == '*' && Text[Index + 1] == '/'))
                {
                    Index++;
                }

                Index++;
                Output.Append(' ');
                continue;
            }

            int RawOpen = Current == '"' && Output.Length > 0 && Output[^1] == 'R' ? Text.IndexOf('(', Index) : -1;

            if (RawOpen >= 0)
            {
                string Close = ")" + Text.Substring(Index + 1, RawOpen - Index - 1) + "\"";
                int RawEnd = Text.IndexOf(Close, RawOpen, StringComparison.Ordinal);
                Index = RawEnd < 0 ? Text.Length : RawEnd + Close.Length - 1;
                Output.Append('0');
                continue;
            }

            if (Current == '"' || (Current == '\'' && !IsDigitSeparator(Output)))
            {
                char Quote = Current;
                Index++;

                while (Index < Text.Length && Text[Index] != Quote)
                {
                    if (Text[Index] == '\\')
                    {
                        Index++;
                    }

                    Index++;
                }

                Output.Append("0");
                continue;
            }

            if (Current == '#' && IsLineStart(Output))
            {
                while (Index < Text.Length && !(Text[Index] == '\n' && !IsContinuedLine(Text, Index)))
                {
                    Index++;
                }

                Output.Append('\n');
                continue;
            }

            Output.Append(Current);
        }

        return Output.ToString();
    }

    private static bool IsLineStart(StringBuilder Output)
    {
        for (int Index = Output.Length - 1; Index >= 0 && Output[Index] != '\n'; Index--)
        {
            if (!char.IsWhiteSpace(Output[Index]))
            {
                return false;
            }
        }

        return true;
    }

    private static bool IsContinuedLine(string Text, int NewlineIndex)
    {
        int Before = NewlineIndex - 1;

        if (Before >= 0 && Text[Before] == '\r')
        {
            Before--;
        }

        return Before >= 0 && Text[Before] == '\\';
    }

    // A quote inside a number token such as 1'000 separates digits rather than opening a character literal.
    private static bool IsDigitSeparator(StringBuilder Output)
    {
        int Start = Output.Length;

        while (Start > 0 && (char.IsAsciiLetterOrDigit(Output[Start - 1]) || Output[Start - 1] is '_' or '\''))
        {
            Start--;
        }

        return Start < Output.Length && char.IsAsciiDigit(Output[Start]);
    }
}
