using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Xml.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace LuminaSharp;

// Matches EQuery on the native side, which sends the number.
internal enum EScriptQuery
{
    Completion = 0,
    Hover = 1,
    Signature = 2,
    Diagnostics = 3,
    Warm = 4,
    Definition = 5,
}

// Answers the code editor's questions about an open script from a Roslyn compilation of its unit, kept warm between queries.
internal sealed class ScriptLanguageService
{
    private const char FieldSeparator = '\u001F';
    private const char RecordSeparator = '\u001E';
    private const int MaxCompletions = 1500;
    private const int MaxDiagnostics = 200;
    private const int InteractiveWaitMs = 30;

    // Mirrors ECodeCompletionKind on the native side.
    private enum ECompletionKind
    {
        Text = 0,
        Keyword = 1,
        Type = 2,
        Function = 3,
        Variable = 4,
        Field = 5,
        Property = 6,
        Value = 7,
        Event = 13,
        Module = 15,
    }

    private sealed class FWorkspace
    {
        public required CSharpCompilation Compilation;
        public required Dictionary<string, SyntaxTree> Trees;
        public GeneratorDriver? Driver;
    }

    private static readonly SymbolDisplayFormat DetailFormat = new(
        globalNamespaceStyle: SymbolDisplayGlobalNamespaceStyle.Omitted,
        typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameOnly,
        genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters,
        memberOptions: SymbolDisplayMemberOptions.IncludeParameters | SymbolDisplayMemberOptions.IncludeType | SymbolDisplayMemberOptions.IncludeRef,
        kindOptions: SymbolDisplayKindOptions.IncludeTypeKeyword,
        propertyStyle: SymbolDisplayPropertyStyle.ShowReadWriteDescriptor,
        parameterOptions: SymbolDisplayParameterOptions.IncludeType | SymbolDisplayParameterOptions.IncludeName
            | SymbolDisplayParameterOptions.IncludeParamsRefOut | SymbolDisplayParameterOptions.IncludeDefaultValue,
        miscellaneousOptions: SymbolDisplayMiscellaneousOptions.UseSpecialTypes | SymbolDisplayMiscellaneousOptions.EscapeKeywordIdentifiers);

    private static readonly SymbolDisplayFormat HoverFormat = new(
        globalNamespaceStyle: SymbolDisplayGlobalNamespaceStyle.Omitted,
        typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameAndContainingTypesAndNamespaces,
        genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters,
        memberOptions: DetailFormat.MemberOptions | SymbolDisplayMemberOptions.IncludeContainingType,
        kindOptions: SymbolDisplayKindOptions.IncludeTypeKeyword,
        propertyStyle: SymbolDisplayPropertyStyle.ShowReadWriteDescriptor,
        parameterOptions: DetailFormat.ParameterOptions,
        miscellaneousOptions: DetailFormat.MiscellaneousOptions);

    private readonly object Lock = new();
    private IReadOnlyList<ScriptAssemblyUnit> Units = Array.Empty<ScriptAssemblyUnit>();
    private Dictionary<string, FWorkspace>? Workspaces;
    private string? LastPath;
    private string? LastText;
    private bool bWarming;

    // A reload brings new sources, so every kept compilation is rebuilt, the open script first.
    public void SetUnits(IReadOnlyList<ScriptAssemblyUnit> InUnits)
    {
        lock (Lock)
        {
            Units = InUnits;
            Workspaces?.Clear();
            if (LastPath is { } OpenPath && LastText is { } OpenText)
            {
                BeginWarm(OpenPath, OpenText);
            }
        }
    }

    // The answer as records of fields, or null while the unit's compilation is still being built.
    public string? Query(EScriptQuery Kind, string FilePath, string Text, int Position)
    {
        bool bInteractive = Kind is EScriptQuery.Completion or EScriptQuery.Hover or EScriptQuery.Signature;
        if (!Monitor.TryEnter(Lock, bInteractive ? InteractiveWaitMs : 0))
        {
            return null;
        }
        try
        {
            LastPath = FilePath;
            LastText = Text;

            // Building a unit's compilation takes seconds, so it happens off the game thread while the editor asks again.
            if (Kind == EScriptQuery.Warm || !HasWorkspace(FilePath))
            {
                BeginWarm(FilePath, Text);
                return Kind == EScriptQuery.Warm ? string.Empty : null;
            }
            return Answer(Kind, FilePath, Text, Math.Clamp(Position, 0, Text.Length));
        }
        finally
        {
            Monitor.Exit(Lock);
        }
    }

    // Runs with Lock held, so only one build is ever in flight.
    private void BeginWarm(string FilePath, string Text)
    {
        if (bWarming)
        {
            return;
        }
        bWarming = true;
        Task.Run(() => Warm(FilePath, Text));
    }

    private bool HasWorkspace(string FilePath)
    {
        string FullPath = Normalize(FilePath);
        return Workspaces != null && Workspaces.ContainsKey(FindUnit(FullPath)?.Name ?? FullPath);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private void Warm(string FilePath, string Text)
    {
        try
        {
            lock (Lock)
            {
                try
                {
                    (CSharpCompilation Compilation, SyntaxTree Tree) = Prepare(FilePath, Text, true);
                    Compilation.GetSemanticModel(Tree).GetDiagnostics();
                }
                finally
                {
                    bWarming = false;
                }
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private string Answer(EScriptQuery Kind, string FilePath, string Text, int Position)
    {
        (CSharpCompilation Compilation, SyntaxTree Tree) = Prepare(FilePath, Text, Kind == EScriptQuery.Diagnostics);
        SemanticModel Model = Compilation.GetSemanticModel(Tree);
        var Out = new StringBuilder();
        switch (Kind)
        {
            case EScriptQuery.Completion:
                WriteCompletions(Model, Tree, Position, Out);
                break;
            case EScriptQuery.Hover:
                WriteHover(Model, Tree, Position, Out);
                break;
            case EScriptQuery.Signature:
                WriteSignature(Model, Tree, Position, Out);
                break;
            case EScriptQuery.Diagnostics:
                WriteDiagnostics(Model, Tree, Out);
                break;
            case EScriptQuery.Definition:
                WriteDefinition(Model, Tree, Position, Out);
                break;
        }
        return Out.ToString();
    }

    private (CSharpCompilation, SyntaxTree) Prepare(string FilePath, string Text, bool bRunGenerators)
    {
        string FullPath = Normalize(FilePath);
        FWorkspace Workspace = GetWorkspace(FullPath);
        SyntaxTree Tree = CSharpSyntaxTree.ParseText(SourceText.From(Text, Encoding.UTF8), ScriptCompiler.ParseOptions, FilePath);
        Workspace.Compilation = Workspace.Trees.TryGetValue(FullPath, out SyntaxTree? Previous)
            ? Workspace.Compilation.ReplaceSyntaxTree(Previous, Tree)
            : Workspace.Compilation.AddSyntaxTrees(Tree);
        Workspace.Trees[FullPath] = Tree;

        if (!bRunGenerators || Workspace.Driver == null)
        {
            return (Workspace.Compilation, Tree);
        }
        Workspace.Driver = Workspace.Driver.RunGeneratorsAndUpdateCompilation(Workspace.Compilation, out Compilation Generated, out _);
        return ((CSharpCompilation)Generated, Tree);
    }

    private FWorkspace GetWorkspace(string FullPath)
    {
        ScriptAssemblyUnit? Unit = FindUnit(FullPath);
        string Key = Unit?.Name ?? FullPath;
        Workspaces ??= new Dictionary<string, FWorkspace>(StringComparer.OrdinalIgnoreCase);
        if (Workspaces.TryGetValue(Key, out FWorkspace? Existing))
        {
            return Existing;
        }

        var Trees = new Dictionary<string, SyntaxTree>(StringComparer.OrdinalIgnoreCase);
        IReadOnlyList<(string Path, string Text)> Sources = Unit?.Sources ?? Array.Empty<(string, string)>();
        foreach ((string SourcePath, string Content) in Sources)
        {
            Trees[Normalize(SourcePath)] = CSharpSyntaxTree.ParseText(SourceText.From(Content, Encoding.UTF8), ScriptCompiler.ParseOptions, SourcePath);
        }

        IEnumerable<SyntaxTree> AllTrees = Trees.Values.Concat(ScriptCompiler.SupportTrees(Key, Sources));
        CSharpCompilation Compilation = CSharpCompilation.Create(Key, AllTrees, ReferencesFor(Unit), ScriptCompiler.CompilationOptions(false));
        ImmutableArray<ISourceGenerator> Generators = ScriptCompiler.SourceGenerators;
        var Created = new FWorkspace
        {
            Compilation = Compilation,
            Trees = Trees,
            Driver = Generators.Length > 0 ? CSharpGeneratorDriver.Create(Generators) : null,
        };
        Workspaces[Key] = Created;
        return Created;
    }

    // The unit listing the file, or for a script not compiled yet the unit whose sources share the most of its folder.
    private ScriptAssemblyUnit? FindUnit(string FullPath)
    {
        ScriptAssemblyUnit? Best = null;
        int BestShared = 0;
        string Folder = Path.GetDirectoryName(FullPath) ?? string.Empty;
        foreach (ScriptAssemblyUnit Unit in Units)
        {
            foreach ((string SourcePath, string _) in Unit.Sources)
            {
                string Source = Normalize(SourcePath);
                if (string.Equals(Source, FullPath, StringComparison.OrdinalIgnoreCase))
                {
                    return Unit;
                }
                int Shared = SharedPrefix(Path.GetDirectoryName(Source) ?? string.Empty, Folder);
                if (Shared > BestShared)
                {
                    BestShared = Shared;
                    Best = Unit;
                }
            }
        }
        return Best;
    }

    private MetadataReference[] ReferencesFor(ScriptAssemblyUnit? Unit)
    {
        var Extra = new List<MetadataReference>();
        if (Unit != null)
        {
            foreach (string Dependency in Unit.Dependencies)
            {
                ScriptAssemblyUnit? Built = Units.FirstOrDefault(Candidate => string.Equals(Candidate.Name, Dependency, StringComparison.OrdinalIgnoreCase));
                // Read into memory, since the next script compile rewrites this file.
                if (Built?.DllPath is { } DllPath && File.Exists(DllPath))
                {
                    Extra.Add(MetadataReference.CreateFromImage(File.ReadAllBytes(DllPath)));
                }
            }
            foreach (string Reference in Unit.References)
            {
                if (File.Exists(Reference))
                {
                    Extra.Add(MetadataReference.CreateFromImage(File.ReadAllBytes(Reference)));
                }
            }
        }
        return ScriptCompiler.ReferencesWith(Extra, true);
    }

    private static void WriteCompletions(SemanticModel Model, SyntaxTree Tree, int Position, StringBuilder Out)
    {
        SyntaxNode Root = Tree.GetRoot();
        SourceText Source = Tree.GetText();
        int WordStart = Position;
        while (WordStart > 0 && IsIdentifierChar(Source[WordStart - 1]))
        {
            WordStart--;
        }
        if (IsInCommentOrString(Root, WordStart))
        {
            return;
        }
        string Word = Source.ToString(TextSpan.FromBounds(WordStart, Position));

        ImmutableArray<ISymbol> Symbols;
        bool bInstanceOnly = false;
        if (WordStart > 0 && Source[WordStart - 1] == '.')
        {
            ExpressionSyntax? Left = LeftOfDot(Root.FindToken(WordStart - 1));
            if (Left == null)
            {
                return;
            }
            ISymbol? LeftSymbol = Model.GetSymbolInfo(Left).Symbol;
            if (LeftSymbol is INamespaceSymbol Namespace)
            {
                Symbols = Model.LookupNamespacesAndTypes(WordStart, Namespace);
            }
            else if (LeftSymbol is ITypeSymbol StaticType)
            {
                Symbols = Model.LookupStaticMembers(WordStart, StaticType);
            }
            else if (Model.GetTypeInfo(Left).Type is { } InstanceType && InstanceType.TypeKind != TypeKind.Error)
            {
                Symbols = Model.LookupSymbols(WordStart, InstanceType, includeReducedExtensionMethods: true);
                bInstanceOnly = true;
            }
            else
            {
                return;
            }
        }
        else
        {
            Symbols = Model.LookupSymbols(WordStart);
        }

        INamedTypeSymbol? Enclosing = Model.GetEnclosingSymbol(WordStart)?.ContainingType;
        var Ranked = Symbols
            .Where(Symbol => IsListable(Symbol) && (!bInstanceOnly || !Symbol.IsStatic || Symbol is IMethodSymbol { MethodKind: MethodKind.ReducedExtension }))
            .Where(Symbol => Fuzzy(Word, Symbol.Name))
            .Select(Symbol => (Symbol, Boost: Boost(Symbol, Enclosing)))
            .OrderByDescending(Entry => Entry.Boost)
            .ToList();

        var Seen = new HashSet<string>(StringComparer.Ordinal);
        int Written = 0;
        foreach ((ISymbol Symbol, int SortBoost) in Ranked)
        {
            if (!Seen.Add(Symbol.Name))
            {
                continue;
            }
            bool bCallable = Symbol is IMethodSymbol;
            AppendRecord(Out,
                Symbol.Name,
                ((int)KindOf(Symbol)).ToString(),
                Symbol.ToDisplayString(DetailFormat),
                Summary(Symbol),
                bCallable ? Symbol.Name + "($0)" : string.Empty,
                SortBoost.ToString());
            if (++Written >= MaxCompletions)
            {
                break;
            }
        }
    }

    private static void WriteHover(SemanticModel Model, SyntaxTree Tree, int Position, StringBuilder Out)
    {
        SyntaxToken Token = TokenAt(Tree.GetRoot(), Position);
        ISymbol? Symbol = SymbolAt(Model, Token);
        if (Symbol == null)
        {
            return;
        }
        string Title = Symbol switch
        {
            ILocalSymbol Local => "(local) " + Local.ToDisplayString(DetailFormat),
            IParameterSymbol Parameter => "(parameter) " + Parameter.ToDisplayString(DetailFormat),
            _ => Symbol.ToDisplayString(HoverFormat),
        };
        (int StartLine, int StartColumn, int EndLine, int EndColumn) = Utf8Span(Tree, Token.Span);
        AppendRecord(Out, Title, Summary(Symbol), StartLine.ToString(), StartColumn.ToString(), EndLine.ToString(), EndColumn.ToString());
    }

    private static void WriteSignature(SemanticModel Model, SyntaxTree Tree, int Position, StringBuilder Out)
    {
        SyntaxToken Token = Tree.GetRoot().FindToken(Math.Max(0, Position - 1));
        BaseArgumentListSyntax? Arguments = Token.Parent?.AncestorsAndSelf().OfType<BaseArgumentListSyntax>().FirstOrDefault(List =>
            List is ArgumentListSyntax Parenthesized
                && Parenthesized.OpenParenToken.Span.End <= Position
                && (Parenthesized.CloseParenToken.IsMissing || Position <= Parenthesized.CloseParenToken.SpanStart));
        if (Arguments?.Parent is not { } Call)
        {
            return;
        }

        var Methods = new List<IMethodSymbol>();
        if (Call is InvocationExpressionSyntax Invocation)
        {
            SymbolInfo Info = Model.GetSymbolInfo(Invocation);
            if (Info.Symbol is IMethodSymbol Bound)
            {
                Methods.Add(Bound);
            }
            Methods.AddRange(Info.CandidateSymbols.OfType<IMethodSymbol>());
            if (Methods.Count == 0)
            {
                Methods.AddRange(Model.GetMemberGroup(Invocation.Expression).OfType<IMethodSymbol>());
            }
        }
        else if (Call is BaseObjectCreationExpressionSyntax && Model.GetTypeInfo(Call).Type is INamedTypeSymbol Created)
        {
            Methods.AddRange(Created.InstanceConstructors.Where(Constructor => Model.IsAccessible(Position, Constructor)));
        }
        if (Methods.Count == 0)
        {
            return;
        }

        int Active = Arguments.Arguments.GetSeparators().Count(Separator => Separator.SpanStart < Position);
        IMethodSymbol Method = Methods.FirstOrDefault(Candidate => Candidate.Parameters.Length > Active || (Candidate.Parameters.LastOrDefault()?.IsParams ?? false)) ?? Methods[0];

        var Label = new StringBuilder();
        var Spans = new List<string>();
        Label.Append(Method.MethodKind == MethodKind.Constructor ? Method.ContainingType.Name : Method.ReturnType.ToDisplayString(DetailFormat) + " " + Method.Name);
        Label.Append('(');
        for (int Index = 0; Index < Method.Parameters.Length; ++Index)
        {
            if (Index > 0)
            {
                Label.Append(", ");
            }
            int Start = Encoding.UTF8.GetByteCount(Label.ToString());
            Label.Append(Method.Parameters[Index].ToDisplayString(DetailFormat));
            Spans.Add(Start + "," + Encoding.UTF8.GetByteCount(Label.ToString()));
        }
        Label.Append(')');

        string Documentation = Summary(Method);
        if (Active < Method.Parameters.Length && ParameterDoc(Method, Method.Parameters[Active].Name) is { Length: > 0 } ParameterText)
        {
            Documentation = Method.Parameters[Active].Name + " is " + ParameterText + (Documentation.Length > 0 ? "\n" + Documentation : string.Empty);
        }
        if (Methods.Count > 1)
        {
            Documentation += (Documentation.Length > 0 ? "\n" : string.Empty) + $"{Methods.Count} overloads";
        }
        AppendRecord(Out, Label.ToString(), Documentation, Active.ToString(), string.Join(";", Spans));
    }

    private static void WriteDiagnostics(SemanticModel Model, SyntaxTree Tree, StringBuilder Out)
    {
        int Written = 0;
        foreach (Diagnostic Diagnostic in Model.GetDiagnostics())
        {
            if (Diagnostic.Severity == DiagnosticSeverity.Hidden || Diagnostic.Location.SourceTree != Tree || ScriptCompiler.IsAssignedByEngine(Diagnostic))
            {
                continue;
            }
            (int StartLine, int StartColumn, int EndLine, int EndColumn) = Utf8Span(Tree, Diagnostic.Location.SourceSpan);
            int Severity = Diagnostic.Severity switch
            {
                DiagnosticSeverity.Error => 0,
                DiagnosticSeverity.Warning => 1,
                _ => 2,
            };
            AppendRecord(Out, StartLine.ToString(), StartColumn.ToString(), EndLine.ToString(), EndColumn.ToString(), Severity.ToString(),
                Diagnostic.Id + " " + Diagnostic.GetMessage());
            if (++Written >= MaxDiagnostics)
            {
                break;
            }
        }
    }

    private static void WriteDefinition(SemanticModel Model, SyntaxTree Tree, int Position, StringBuilder Out)
    {
        ISymbol? Symbol = SymbolAt(Model, TokenAt(Tree.GetRoot(), Position));
        Location? Declared = Symbol?.Locations.FirstOrDefault(Location => Location.IsInSource && Location.SourceTree is { FilePath.Length: > 0 });
        if (Declared?.SourceTree is not { } DeclaringTree)
        {
            return;
        }
        (int Line, int Column, int _, int _) = Utf8Span(DeclaringTree, Declared.SourceSpan);
        AppendRecord(Out, DeclaringTree.FilePath, Line.ToString(), Column.ToString());
    }

    private static SyntaxToken TokenAt(SyntaxNode Root, int Position)
    {
        SyntaxToken Token = Root.FindToken(Position);
        if (!Token.Span.Contains(Position) && Position > 0)
        {
            Token = Root.FindToken(Position - 1);
        }
        return Token;
    }

    private static ISymbol? SymbolAt(SemanticModel Model, SyntaxToken Token)
    {
        if (Token.Parent is not { } Node)
        {
            return null;
        }
        if (Token.IsKind(SyntaxKind.IdentifierToken) && Model.GetDeclaredSymbol(Node) is { } Declared)
        {
            return Declared;
        }
        SymbolInfo Info = Model.GetSymbolInfo(Node);
        return Info.Symbol ?? Info.CandidateSymbols.FirstOrDefault() ?? Model.GetTypeInfo(Node).Type as ISymbol;
    }

    private static ExpressionSyntax? LeftOfDot(SyntaxToken Dot)
    {
        return Dot.Parent switch
        {
            MemberAccessExpressionSyntax Access => Access.Expression,
            QualifiedNameSyntax Qualified => Qualified.Left,
            MemberBindingExpressionSyntax Binding when Binding.Parent?.Parent is ConditionalAccessExpressionSyntax Conditional => Conditional.Expression,
            _ => null,
        };
    }

    private static bool IsListable(ISymbol Symbol)
    {
        if (Symbol.Name.Length == 0 || Symbol.Name[0] == '<' || (Symbol.IsImplicitlyDeclared && Symbol is not IParameterSymbol))
        {
            return false;
        }
        return Symbol is not IMethodSymbol Method
            || Method.MethodKind is MethodKind.Ordinary or MethodKind.ReducedExtension or MethodKind.LocalFunction;
    }

    private static int Boost(ISymbol Symbol, INamedTypeSymbol? Enclosing)
    {
        if (Symbol is ILocalSymbol or IParameterSymbol or IRangeVariableSymbol)
        {
            return 30;
        }
        if (Enclosing != null && SymbolEqualityComparer.Default.Equals(Symbol.ContainingType, Enclosing))
        {
            return 20;
        }
        return Symbol is INamespaceSymbol ? -5 : 0;
    }

    private static ECompletionKind KindOf(ISymbol Symbol)
    {
        return Symbol switch
        {
            ILocalSymbol or IParameterSymbol or IRangeVariableSymbol => ECompletionKind.Variable,
            IFieldSymbol Field => Field.ContainingType?.TypeKind == TypeKind.Enum ? ECompletionKind.Value : ECompletionKind.Field,
            IPropertySymbol => ECompletionKind.Property,
            IMethodSymbol => ECompletionKind.Function,
            IEventSymbol => ECompletionKind.Event,
            ITypeSymbol => ECompletionKind.Type,
            INamespaceSymbol => ECompletionKind.Module,
            _ => ECompletionKind.Text,
        };
    }

    // The summary text of the symbol's documentation, empty when it has none.
    private static string Summary(ISymbol Symbol)
    {
        XElement? Root = DocumentationOf(Symbol);
        if (Root == null)
        {
            return string.Empty;
        }
        XElement? Summary = Root.Element("summary");
        return Collapse(Summary != null ? Summary.Value : Root.Elements().Any() ? string.Empty : Root.Value);
    }

    private static string ParameterDoc(IMethodSymbol Method, string Name)
    {
        XElement? Root = DocumentationOf(Method);
        XElement? Parameter = Root?.Elements("param").FirstOrDefault(Element => (string?)Element.Attribute("name") == Name);
        return Parameter != null ? Collapse(Parameter.Value) : string.Empty;
    }

    private static XElement? DocumentationOf(ISymbol Symbol)
    {
        ISymbol Documented = Symbol is IMethodSymbol { ReducedFrom: { } Original } ? Original : Symbol;
        string Xml = Documented.GetDocumentationCommentXml() ?? string.Empty;
        if (Xml.Length == 0)
        {
            return null;
        }
        try
        {
            return XElement.Parse(Xml.TrimStart().StartsWith("<member", StringComparison.Ordinal) ? Xml : "<member>" + Xml + "</member>");
        }
        catch (System.Xml.XmlException)
        {
            return null;
        }
    }

    private static string Collapse(string Text)
    {
        return string.Join(' ', Text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries));
    }

    private static bool IsInCommentOrString(SyntaxNode Root, int Position)
    {
        if (Position <= 0)
        {
            return false;
        }
        SyntaxTrivia Trivia = Root.FindTrivia(Position - 1);
        if (Trivia.IsKind(SyntaxKind.SingleLineCommentTrivia) || Trivia.IsKind(SyntaxKind.MultiLineCommentTrivia)
            || Trivia.IsKind(SyntaxKind.SingleLineDocumentationCommentTrivia) || Trivia.IsKind(SyntaxKind.MultiLineDocumentationCommentTrivia))
        {
            return true;
        }
        SyntaxToken Token = Root.FindToken(Position - 1);
        return Token.Span.Contains(Position - 1) && (Token.IsKind(SyntaxKind.StringLiteralToken) || Token.IsKind(SyntaxKind.CharacterLiteralToken)
            || Token.IsKind(SyntaxKind.InterpolatedStringTextToken));
    }

    private static bool IsIdentifierChar(char C)
    {
        return char.IsLetterOrDigit(C) || C == '_';
    }

    // Case-insensitive subsequence, the same test the editor filters with, so nothing it would show is dropped here.
    private static bool Fuzzy(string Word, string Name)
    {
        int Matched = 0;
        for (int Index = 0; Index < Name.Length && Matched < Word.Length; ++Index)
        {
            if (char.ToLowerInvariant(Name[Index]) == char.ToLowerInvariant(Word[Matched]))
            {
                ++Matched;
            }
        }
        return Matched == Word.Length;
    }

    // The span as zero based lines and UTF-8 byte columns, which is how the editor addresses text.
    private static (int, int, int, int) Utf8Span(SyntaxTree Tree, TextSpan Span)
    {
        SourceText Text = Tree.GetText();
        LinePositionSpan Lines = Text.Lines.GetLinePositionSpan(Span);
        return (Lines.Start.Line, Utf8Column(Text, Lines.Start), Lines.End.Line, Utf8Column(Text, Lines.End));
    }

    private static int Utf8Column(SourceText Text, LinePosition Position)
    {
        TextLine Line = Text.Lines[Position.Line];
        return Encoding.UTF8.GetByteCount(Text.ToString(new TextSpan(Line.Start, Math.Min(Position.Character, Line.Span.Length))));
    }

    private static void AppendRecord(StringBuilder Out, params string[] Fields)
    {
        if (Out.Length > 0)
        {
            Out.Append(RecordSeparator);
        }
        for (int Index = 0; Index < Fields.Length; ++Index)
        {
            if (Index > 0)
            {
                Out.Append(FieldSeparator);
            }
            Out.Append(Fields[Index].Replace(FieldSeparator, ' ').Replace(RecordSeparator, ' '));
        }
    }

    private static string Normalize(string FilePath)
    {
        try
        {
            return Path.GetFullPath(FilePath);
        }
        catch (Exception)
        {
            return FilePath;
        }
    }

    private static int SharedPrefix(string A, string B)
    {
        int Length = Math.Min(A.Length, B.Length);
        int Index = 0;
        while (Index < Length && char.ToLowerInvariant(A[Index]) == char.ToLowerInvariant(B[Index]))
        {
            ++Index;
        }
        return Index;
    }
}

// Reads an assembly's XML documentation file, since the provider Roslyn ships lives in the Workspaces package scripts do not reference.
internal sealed class FileDocumentationProvider : DocumentationProvider
{
    private readonly string FilePath;
    private readonly Dictionary<string, string> Members = new(StringComparer.Ordinal);

    public FileDocumentationProvider(string InFilePath)
    {
        FilePath = InFilePath;
        try
        {
            foreach (XElement Member in XDocument.Load(InFilePath).Descendants("member"))
            {
                if ((string?)Member.Attribute("name") is { } Name)
                {
                    Members[Name] = Member.ToString(SaveOptions.DisableFormatting);
                }
            }
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Warn, $"Script documentation '{InFilePath}' could not be read: {Exception.Message}");
        }
    }

    protected override string GetDocumentationForSymbol(string DocumentationMemberID, System.Globalization.CultureInfo PreferredCulture, CancellationToken CancellationToken)
    {
        return Members.TryGetValue(DocumentationMemberID, out string? Xml) ? Xml : string.Empty;
    }

    public override bool Equals(object? Other)
    {
        return Other is FileDocumentationProvider Provider && string.Equals(Provider.FilePath, FilePath, StringComparison.OrdinalIgnoreCase);
    }

    public override int GetHashCode()
    {
        return StringComparer.OrdinalIgnoreCase.GetHashCode(FilePath);
    }
}
