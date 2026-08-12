using System.Collections.Generic;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace LuminaSharp;

/// <summary>
/// Rewrites <c>[Property] public float Speed = 5.0f;</c> into a property whose accessors read and write the
/// NATIVE storage the engine minted for that member, and moves the initializer to where a default belongs.
///
/// Why a rewriter and not a source generator. A script property's value has to live in native memory: it is
/// what the inspector, the tagged serializer, undo, prefab overrides and replication all read, and they read
/// it through a pointer at <c>Container + Offset</c> while the managed instance may not even exist yet. So the
/// C# side must go through that pointer, which a field cannot do -- only an accessor can. A source generator
/// may only ADD members, never replace one, which is what forced the author to write
/// <c>public partial float Speed { get; set; }</c> and left nowhere to put a default (a partial property may
/// not have an initializer).
///
/// None of that constrains US, because the engine compiles scripts itself: <see cref="ScriptCompiler"/> owns
/// the syntax trees before they are compiled, and the assembly it emits is the only one ever loaded -- the
/// generated .csproj is IntelliSense-only, and packaging stages this compiler's output. So the field is simply
/// turned into the property it needs to be, and the author writes ordinary C#.
///
/// The initializer becomes a default rather than a constructor store. A constructor store would run when the
/// managed wrapper is created, which is AFTER the native object has been loaded from a scene -- it would
/// overwrite every authored value with the declared default. Instead each class gets a generated
/// <c>__ApplyScriptDefaults</c> which the engine runs once against the class default object; every instance is
/// then copied from that, exactly as a C++ class gets its defaults from its constructor via its CDO.
/// </summary>
internal static class ScriptPropertyRewriter
{
    /// <summary>Rewrites one tree against a compilation used only to classify member types. Returns the tree
    /// unchanged when it declares no [Property] fields.</summary>
    public static SyntaxTree Rewrite(CSharpCompilation Probe, SyntaxTree Tree, List<string> OutErrors)
    {
        SyntaxNode Root = Tree.GetRoot();
        if (!Root.DescendantNodes().OfType<FieldDeclarationSyntax>().Any(HasPropertyAttribute))
        {
            return Tree;
        }

        var Walker = new Rewriter(Probe.GetSemanticModel(Tree), Tree.FilePath, OutErrors);
        SyntaxNode Rewritten = Walker.Visit(Root);
        if (OutErrors.Count > 0)
        {
            return Tree;
        }

        // The original path is kept so #line directives (emitted per member) resolve, and so a diagnostic
        // Roslyn raises anywhere else in the file still names the file the author wrote.
        return CSharpSyntaxTree.Create((CSharpSyntaxNode)Rewritten, (CSharpParseOptions?)Tree.Options, Tree.FilePath, Encoding.UTF8);
    }

    private static bool HasPropertyAttribute(FieldDeclarationSyntax Field)
    {
        return Field.AttributeLists
            .SelectMany(List => List.Attributes)
            .Any(Attribute =>
            {
                string Name = Attribute.Name.ToString();
                int Dot = Name.LastIndexOf('.');
                if (Dot >= 0)
                {
                    Name = Name.Substring(Dot + 1);
                }
                return Name == "Property" || Name == "PropertyAttribute";
            });
    }

    // How one member's value is reached; mirrors the native property kinds. See EmitAccessor.
    private enum EAccess
    {
        Unsupported,
        Blittable,
        Enum,
        String,
        AssetPath,
        Object,
        View,
    }

    private sealed class Rewriter : CSharpSyntaxRewriter
    {
        private readonly SemanticModel Model;
        private readonly string FilePath;
        private readonly List<string> Errors;

        public Rewriter(SemanticModel Model, string FilePath, List<string> Errors)
        {
            this.Model = Model;
            this.FilePath = FilePath;
            this.Errors = Errors;
        }

        public override SyntaxNode? VisitClassDeclaration(ClassDeclarationSyntax Node)
        {
            List<FieldDeclarationSyntax> Fields = Node.Members.OfType<FieldDeclarationSyntax>()
                .Where(HasPropertyAttribute).ToList();
            if (Fields.Count == 0)
            {
                return base.VisitClassDeclaration(Node);
            }

            string TypeName = Model.GetDeclaredSymbol(Node)?.ToDisplayString() ?? Node.Identifier.Text;

            // Keyed by field, because one field can declare several members (`float A, B;`) and each expands
            // to several: two lazy-resolve statics plus the property itself.
            var Replacements = new Dictionary<FieldDeclarationSyntax, List<MemberDeclarationSyntax>>();
            var Defaults = new List<string>();

            foreach (FieldDeclarationSyntax Field in Fields)
            {
                foreach (VariableDeclaratorSyntax Declarator in Field.Declaration.Variables)
                {
                    if (Model.GetDeclaredSymbol(Declarator) is not IFieldSymbol Symbol)
                    {
                        continue;
                    }

                    EAccess Access = Classify(Symbol.Type, out ITypeSymbol? Element);
                    if (Access == EAccess.Unsupported)
                    {
                        Errors.Add($"{FilePath}({Line(Declarator)}): [Property] '{Symbol.Name}' has type "
                                 + $"'{Symbol.Type.ToDisplayString()}', which cannot be viewed over native storage. "
                                 + "Supported: numbers, bool, enums, blittable struct mirrors, string, "
                                 + "Lumina.FSoftObjectPath, NativeObject-derived references, and NativeList<T> of an unmanaged T.");
                        continue;
                    }

                    // A container is a view over storage native owns, so it has no setter and no default:
                    // assigning it is meaningless while its contents are fully editable.
                    if (Access == EAccess.View && Declarator.Initializer != null)
                    {
                        Errors.Add($"{FilePath}({Line(Declarator)}): [Property] '{Symbol.Name}' is a container. It is a "
                                 + "view over the native storage, so it cannot be initialized; add to it in OnReady instead.");
                        continue;
                    }

                    if (Declarator.Initializer != null)
                    {
                        Defaults.Add($"{Symbol.Name} = {Declarator.Initializer.Value};");
                    }

                    if (!Replacements.TryGetValue(Field, out List<MemberDeclarationSyntax>? Built))
                    {
                        Built = new List<MemberDeclarationSyntax>();
                        Replacements[Field] = Built;
                    }
                    Built.AddRange(BuildProperty(Field, Declarator, Symbol, TypeName, Access, Element));
                }
            }

            if (Errors.Count > 0)
            {
                return Node;
            }

            // The declared order is preserved: each field is replaced in place by its property, so the
            // inspector's row order still follows the source.
            var Members = new List<MemberDeclarationSyntax>();
            foreach (MemberDeclarationSyntax Member in Node.Members)
            {
                if (Member is FieldDeclarationSyntax Field && Replacements.TryGetValue(Field, out List<MemberDeclarationSyntax>? Built))
                {
                    Members.AddRange(Built);
                    continue;
                }
                Members.Add(Member);
            }

            if (Defaults.Count > 0)
            {
                Members.AddRange(BuildDefaultsMethod(Defaults));
            }

            return Node.WithMembers(SyntaxFactory.List(Members));
        }

        private int Line(SyntaxNode Node) => Node.GetLocation().GetLineSpan().StartLinePosition.Line + 1;

        /// <summary>
        /// The property replacing one field. Wrapped in #line directives pointing back at the field, so a
        /// stack trace, a breakpoint and any later diagnostic in the file still name the line the author
        /// wrote rather than an offset into generated text.
        /// </summary>
        private IEnumerable<MemberDeclarationSyntax> BuildProperty(FieldDeclarationSyntax Field, VariableDeclaratorSyntax Declarator,
            IFieldSymbol Symbol, string TypeName, EAccess Access, ITypeSymbol? Element)
        {
            string Name = Symbol.Name;
            string Type = Symbol.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
            string Offset = $"__lazyoff_{Name}.Get(\"{TypeName}\", \"{Name}\")";
            string Token = $"__lazyprop_{Name}.Get(\"{TypeName}\", \"{Name}\")";
            bool bWritable = Access != EAccess.View && !Symbol.IsReadOnly;

            string Get;
            string? Set = null;
            string Unbound = "default";

            switch (Access)
            {
                case EAccess.Blittable:
                    Get = $"global::System.Runtime.CompilerServices.Unsafe.ReadUnaligned<{Type}>((void*)((nint)Handle + {Offset}))";
                    Set = $"global::System.Runtime.CompilerServices.Unsafe.WriteUnaligned((void*)((nint)Handle + {Offset}), value)";
                    break;

                // The minted enum property is int64-wide whatever the C# underlying type is, so the whole slot
                // is read and written as a long -- a 4-byte access would leave the top half stale on writes.
                case EAccess.Enum:
                    Get = $"({Type})global::System.Runtime.CompilerServices.Unsafe.ReadUnaligned<long>((void*)((nint)Handle + {Offset}))";
                    Set = $"global::System.Runtime.CompilerServices.Unsafe.WriteUnaligned<long>((void*)((nint)Handle + {Offset}), (long)value)";
                    break;

                case EAccess.String:
                    Get = $"global::LuminaSharp.NativeMarshal.ReadString((nint)Handle + {Offset})";
                    Set = $"global::LuminaSharp.Native.PropSetString(Handle, {Token}, value)";
                    Unbound = "\"\"";
                    break;

                case EAccess.AssetPath:
                    Get = $"new {Type}(global::LuminaSharp.Native.PropGetAssetPath(Handle, {Token}))";
                    Set = $"global::LuminaSharp.Native.PropSetAssetPath(Handle, {Token}, value.Path)";
                    break;

                // The canonical wrapper, so reading twice returns the same instance and reference equality
                // means what a script author expects.
                case EAccess.Object:
                    Get = $"global::LuminaSharp.Wrapper<{Type}>.ForObject(global::LuminaSharp.Native.PropGetObject(Handle, {Token}))";
                    Set = $"global::LuminaSharp.Native.PropSetObject(Handle, {Token}, value is null ? System.IntPtr.Zero : value.Handle)";
                    break;

                default:
                {
                    string View = $"global::LuminaSharp.NativeList<{Element!.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)}>";
                    Type = View;
                    Get = $"new {View}((nint)Handle + {Offset}, (nint)global::LuminaSharp.Native.PropVectorOps({Token}))";
                    break;
                }
            }

            var Builder = new StringBuilder();
            Builder.Append("private static global::LuminaSharp.LazyPropertyOffset __lazyoff_").Append(Name).AppendLine(";");
            Builder.Append("private static global::LuminaSharp.LazyPropertyToken __lazyprop_").Append(Name).AppendLine(";");
            // The attributes come across verbatim, and they are not decoration: TypeLibrary discovers a
            // member BY [Property] at run time, and reads Category/Tooltip/Min/Max off it to build the
            // inspector row. Dropping them here would compile perfectly and publish nothing.
            foreach (AttributeListSyntax List in Field.AttributeLists)
            {
                Builder.AppendLine(List.ToString());
            }
            Builder.Append(Accessibility(Field)).Append(' ').Append(Type).Append(' ').AppendLine(Name);
            Builder.AppendLine("{");
            // Gated on HasNativeStorage: the schema pass creates one UNBOUND instance per script type purely
            // to describe it, and reading through a null handle there is an access violation on load.
            Builder.Append("    get => HasNativeStorage ? ").Append(Get).Append(" : ").Append(Unbound).AppendLine(";");
            if (bWritable && Set != null)
            {
                Builder.Append("    set { if (HasNativeStorage) { ").Append(Set).AppendLine("; } }");
            }
            Builder.AppendLine("}");
            // Parsed as a class body rather than with ParseMemberDeclaration, which returns only the FIRST
            // member it finds -- that silently dropped the property and kept just the offset static.
            List<MemberDeclarationSyntax> Members = ParseMembers(Builder.ToString()).ToList();
            if (Members.Count == 0)
            {
                return Members;
            }

            // #line, attached as trivia rather than written into the parsed text: inside a wrapper class the
            // directive binds to the class's own brace token, so extracting the members drops it silently.
            //
            // The first maps the expansion back to the field the author wrote; the second hands the counter
            // to the line just past it, so every LATER line in the file keeps its real number instead of
            // drifting by however many lines this expanded to. Without them a stack trace from script code
            // names the wrong statement.
            const int OneBased = 1;
            const int NextLine = 1;
            Members[0] = Members[0].WithLeadingTrivia(Directive(Line(Declarator)));
            Members[^1] = Members[^1].WithTrailingTrivia(
                Members[^1].GetTrailingTrivia().AddRange(
                    Directive(Field.GetLocation().GetLineSpan().EndLinePosition.Line + OneBased + NextLine)));
            return Members;
        }

        private SyntaxTriviaList Directive(int SourceLine)
        {
            return SyntaxFactory.ParseLeadingTrivia($"#line {SourceLine} \"{FilePath.Replace("\\", "\\\\")}\"\n");
        }

        private static IEnumerable<MemberDeclarationSyntax> ParseMembers(string Text)
        {
            // The newlines matter: a #line directive is only recognised at the START of a line, so splicing
            // the wrapper onto the same line silently demotes it to nothing and the mapping is lost.
            CompilationUnitSyntax Unit = SyntaxFactory.ParseCompilationUnit("class __Members__ {\n" + Text + "\n}");
            return Unit.Members.OfType<ClassDeclarationSyntax>().SelectMany(Class => Class.Members);
        }

        /// <summary>The class's declared initializers, replayed against the class default object. Not a
        /// constructor: the managed wrapper is created AFTER the native object is loaded, so assigning there
        /// would overwrite every authored value with the declared default.</summary>
        private static IEnumerable<MemberDeclarationSyntax> BuildDefaultsMethod(List<string> Defaults)
        {
            var Builder = new StringBuilder();
            // 'protected', not 'protected internal': the base member is protected internal in ANOTHER
            // assembly, and internal does not cross assemblies, so C# requires the override to narrow.
            Builder.AppendLine("protected override void __ApplyScriptDefaults()");
            Builder.AppendLine("{");
            Builder.AppendLine("    base.__ApplyScriptDefaults();");
            foreach (string Assignment in Defaults)
            {
                Builder.Append("    ").AppendLine(Assignment);
            }
            Builder.AppendLine("}");
            return ParseMembers(Builder.ToString());
        }

        private static string Accessibility(FieldDeclarationSyntax Field)
        {
            // 'unsafe' because every accessor dereferences the container pointer; the field's own modifiers
            // (public/internal) carry over so the property is reachable exactly where the field was.
            List<string> Modifiers = Field.Modifiers
                .Select(Token => Token.Text)
                .Where(Text => Text is "public" or "private" or "protected" or "internal")
                .ToList();
            if (Modifiers.Count == 0)
            {
                Modifiers.Add("private");
            }
            Modifiers.Add("unsafe");
            return string.Join(" ", Modifiers);
        }

        private static EAccess Classify(ITypeSymbol Type, out ITypeSymbol? Element)
        {
            Element = null;

            if (Type.TypeKind == TypeKind.Enum)
            {
                return EAccess.Enum;
            }
            if (Type.SpecialType == SpecialType.System_String)
            {
                return EAccess.String;
            }
            if (Type.ToDisplayString() == "Lumina.FSoftObjectPath")
            {
                return EAccess.AssetPath;
            }
            for (INamedTypeSymbol? Base = Type.BaseType; Base != null; Base = Base.BaseType)
            {
                if (Base.ToDisplayString() == "LuminaSharp.NativeObject")
                {
                    return EAccess.Object;
                }
            }
            if (Type is INamedTypeSymbol Named && Named.IsGenericType
                && Named.ConstructedFrom.ToDisplayString() == "LuminaSharp.NativeList<T>")
            {
                Element = Named.TypeArguments[0];
                return Element.IsUnmanagedType ? EAccess.View : EAccess.Unsupported;
            }

            // Everything blittable (numbers, bool, and the struct mirrors like FVector3/FTransform) is read in
            // place. Checked last so the special cases above win.
            return Type.IsUnmanagedType ? EAccess.Blittable : EAccess.Unsupported;
        }
    }
}
