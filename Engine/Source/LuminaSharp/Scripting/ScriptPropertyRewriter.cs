using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using LuminaSharp.ScriptProperties;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace LuminaSharp;

// Turns each stored field into a property over its native storage, and its initializer into a default run once against the CDO.
internal static class ScriptPropertyRewriter
{
    // Rewrites one tree against a compilation used only to classify member types, unchanged when nothing needs native storage.
    public static SyntaxTree Rewrite(CSharpCompilation Probe, SyntaxTree Tree, List<string> OutErrors)
    {
        SyntaxNode Root = Tree.GetRoot();
        if (!Root.DescendantNodes().OfType<FieldDeclarationSyntax>().Any(Field => MayNeedNativeStorage(Field.AttributeLists))
            && !Root.DescendantNodes().OfType<PropertyDeclarationSyntax>().Any(Property => MayNeedNativeStorage(Property.AttributeLists))
            && !Root.DescendantNodes().OfType<MethodDeclarationSyntax>().Any(MayBeRpc))
        {
            return Tree;
        }

        var Walker = new Rewriter(Probe.GetSemanticModel(Tree), Tree.FilePath, OutErrors);
        SyntaxNode Rewritten = Walker.Visit(Root);
        if (OutErrors.Count > 0)
        {
            return Tree;
        }

        // The original path is kept, so #line directives resolve and every other diagnostic still names the author's file.
        return CSharpSyntaxTree.Create((CSharpSyntaxNode)Rewritten, (CSharpParseOptions?)Tree.Options, Tree.FilePath, Encoding.UTF8);
    }

    // A syntax-only screen, so a file with no [Property] anywhere costs no semantic model. ScriptMemberRules decides.
    private static bool MayNeedNativeStorage(SyntaxList<AttributeListSyntax> AttributeLists)
    {
        return AttributeLists.SelectMany(List => List.Attributes).Any(Attribute =>
        {
            string Name = Attribute.Name.ToString();
            int Dot = Name.LastIndexOf('.');
            Name = Dot >= 0 ? Name.Substring(Dot + 1) : Name;
            return Name is "Property" or ScriptMemberRules.PropertyAttribute;
        });
    }

    private static bool HasSpecifier(ISymbol Member, string Specifier)
    {
        return ScriptPropertyClassifier.SpecifiersOf(Member, ScriptPropertyTypeNames.PropertyAttribute)?.Contains(Specifier) == true;
    }

    // Syntax only, so the cheap pre-check costs no semantic model. The semantic pass confirms each one.
    private static bool MayBeRpc(MethodDeclarationSyntax Method)
    {
        return Method.AttributeLists.SelectMany(List => List.Attributes).Any(Attribute =>
        {
            string Name = Attribute.Name.ToString();
            return Name.Contains("Rpc.") || Name.Contains("RpcAttribute");
        });
    }

    private sealed class Rewriter : CSharpSyntaxRewriter
    {
        private readonly SemanticModel Model;
        private readonly string FilePath;
        private readonly List<string> Errors;
        private readonly INamedTypeSymbol? NativeObjectSymbol;
        private readonly INamedTypeSymbol? EntityScriptSymbol;

        // How an accessor reaches its owner's memory, the base wrapper's handle for a class and its own for a script struct.
        private string SelfHandle = "base.Handle";
        private bool bStructOwner;
        private bool bComponentOwner;

        public Rewriter(SemanticModel Model, string FilePath, List<string> Errors)
        {
            this.Model = Model;
            this.FilePath = FilePath;
            this.Errors = Errors;
            NativeObjectSymbol = Model.Compilation.GetTypeByMetadataName(typeof(NativeObject).FullName!);
            EntityScriptSymbol = Model.Compilation.GetTypeByMetadataName(typeof(Lumina.CEntityScript).FullName!);
        }

        public override SyntaxNode? VisitClassDeclaration(ClassDeclarationSyntax Node)
        {
            bool bNativeBacked = DerivesFrom(Model.GetDeclaredSymbol(Node), NativeObjectSymbol);
            List<FStoredMember> Stored = CollectStoredMembers(Node, bNativeBacked);
            List<FRpcMethod> Rpcs = CollectRpcs(Node);
            if (Stored.Count == 0 && Rpcs.Count == 0)
            {
                return base.VisitClassDeclaration(Node);
            }
            return RewriteClass(Node, Stored, Rpcs);
        }

        // A [Reflect] root, native struct extension or component is a handle over memory native mints from its fields.
        public override SyntaxNode? VisitStructDeclaration(StructDeclarationSyntax Node)
        {
            if (Model.GetDeclaredSymbol(Node) is not INamedTypeSymbol Symbol || !IsNativeBackedStruct(Symbol))
            {
                return base.VisitStructDeclaration(Node);
            }

            string Where = $"{FilePath}({Line(Node)}): script struct '{Symbol.Name}'";
            if (!Node.Modifiers.Any(SyntaxKind.PartialKeyword))
            {
                Errors.Add($"{Where} must be partial, since the engine adds the handle it views native memory through.");
                return Node;
            }

            var Stored = new List<FStoredMember>();
            foreach (MemberDeclarationSyntax Member in Node.Members)
            {
                if (Member is not FieldDeclarationSyntax Field || Field.Modifiers.Any(SyntaxKind.StaticKeyword) || Field.Modifiers.Any(SyntaxKind.ConstKeyword))
                {
                    continue;
                }
                if (!MayNeedNativeStorage(Field.AttributeLists) || !IsStored(Field, bNativeBacked: true))
                {
                    Errors.Add($"{FilePath}({Line(Field)}): '{Field.Declaration.Variables.First().Identifier.Text}' in script struct '{Symbol.Name}' "
                             + "has no [Property]. A script struct is a view over native memory, so every field has to be one the engine stores.");
                    continue;
                }
                foreach (VariableDeclaratorSyntax Declarator in Field.Declaration.Variables)
                {
                    if (Model.GetDeclaredSymbol(Declarator) is IFieldSymbol FieldSymbol)
                    {
                        Stored.Add(new FStoredMember
                        {
                            Declaration = Field,
                            At = Declarator,
                            Symbol = FieldSymbol,
                            Type = FieldSymbol.Type,
                            Initializer = Declarator.Initializer,
                            bReadOnly = FieldSymbol.IsReadOnly,
                        });
                    }
                }
            }
            if (Errors.Count > 0)
            {
                return Node;
            }

            string PreviousHandle = SelfHandle;
            bool bPreviousStruct = bStructOwner;
            bool bPreviousComponent = bComponentOwner;
            SelfHandle = "Handle";
            bStructOwner = true;
            bComponentOwner = IsComponent(Symbol);
            try
            {
                return RewriteScriptStruct(Node, Symbol, Stored);
            }
            finally
            {
                SelfHandle = PreviousHandle;
                bStructOwner = bPreviousStruct;
                bComponentOwner = bPreviousComponent;
            }
        }

        private static bool IsComponent(INamedTypeSymbol Symbol)
        {
            return Symbol.AllInterfaces.Any(Interface => Interface.OriginalDefinition.ToDisplayString() == "LuminaSharp.IComponent<TSelf>");
        }

        private static bool IsNativeBackedStruct(INamedTypeSymbol Symbol)
        {
            bool bReflected = Symbol.GetAttributes().Any(Data => Data.AttributeClass?.ToDisplayString() == "LuminaSharp.ReflectAttribute");
            bool bHandle = Symbol.AllInterfaces.Any(Interface => Interface.OriginalDefinition.ToDisplayString() is "LuminaSharp.IExtends<TBase>" or "LuminaSharp.IComponent<TSelf>"
                || (Interface.OriginalDefinition.ToDisplayString() == "LuminaSharp.INativeStruct<TSelf>" && SymbolEqualityComparer.Default.Equals(Interface.TypeArguments[0], Symbol)));
            return bReflected && bHandle;
        }

        private SyntaxNode RewriteScriptStruct(StructDeclarationSyntax Node, INamedTypeSymbol Symbol, List<FStoredMember> Stored)
        {
            string TypeName = Symbol.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
            var Replacements = new Dictionary<MemberDeclarationSyntax, List<MemberDeclarationSyntax>>();
            var Defaults = new List<string>();

            foreach (FStoredMember Member in Stored)
            {
                string MemberName = Member.Symbol.Name;
                FScriptPropertyClassification Classification = ScriptPropertyClassifier.ClassifyMember(Member.Symbol, Member.Type);
                if (!Classification.IsSupported)
                {
                    Errors.Add($"{FilePath}({Line(Member.At)}): [Property] '{MemberName}' has type '{Member.Type.ToDisplayString()}': {Classification.Rejection}");
                    continue;
                }
                if (Classification.Access == EScriptAccess.ComponentRef)
                {
                    Errors.Add($"{FilePath}({Line(Member.At)}): [Property(Component)] '{MemberName}' is on a script struct, which has no entity to find the component on.");
                    continue;
                }
                if (HasSpecifier(Member.Symbol, ScriptMemberRules.Sync))
                {
                    Errors.Add($"{FilePath}({Line(Member.At)}): [Property(Sync)] '{MemberName}' is on a script struct, which has no entity to replicate through.");
                    continue;
                }
                if (Classification.IsView && Member.Initializer != null)
                {
                    Errors.Add($"{FilePath}({Line(Member.At)}): [Property] '{MemberName}' is a container. It is a view over the native storage, so it cannot be initialized.");
                    continue;
                }
                if (Member.Initializer != null)
                {
                    Defaults.Add($"__Self.{MemberName} = {Member.Initializer.Value};");
                }
                (CollectionsMarshal.GetValueRefOrAddDefault(Replacements, Member.Declaration, out _) ??= new()).AddRange(BuildProperty(Member, TypeName, Classification));
            }
            if (Errors.Count > 0)
            {
                return Node;
            }

            // ScriptStructGenerator adds the handle itself, so only the defaults are left to write here.
            var Handle = new StringBuilder();
            if (bComponentOwner)
            {
                // A component is its storage, so the defaults write through a ref to it rather than a handle.
                Handle.AppendLine("internal static unsafe void __ApplyScriptDefaults(nint Address)");
                Handle.AppendLine("{");
                Handle.Append("    ref ").Append(TypeName).Append(" __Self = ref global::System.Runtime.CompilerServices.Unsafe.AsRef<")
                      .Append(TypeName).AppendLine(">((void*)Address);");
            }
            else
            {
                Handle.AppendLine("internal static void __ApplyScriptDefaults(global::LuminaSharp.NativeRef Ref)");
                Handle.AppendLine("{");
                Handle.Append("    ").Append(TypeName).Append(" __Self = new ").Append(TypeName).AppendLine("(Ref);");
            }
            foreach (string Assignment in Defaults)
            {
                Handle.Append("    ").AppendLine(Assignment);
            }
            Handle.AppendLine("}");

            var Members = new List<MemberDeclarationSyntax>(ParseMembers(Handle.ToString()));
            foreach (MemberDeclarationSyntax Member in Node.Members)
            {
                if (Replacements.TryGetValue(Member, out List<MemberDeclarationSyntax>? Built))
                {
                    Members.AddRange(Built);
                    continue;
                }
                Members.Add(Member);
            }

            return Node.WithMembers(SyntaxFactory.List(Members));
        }

        // One member whose value lives in native storage, a field declarator or an auto-property.
        private sealed class FStoredMember
        {
            public required MemberDeclarationSyntax Declaration;
            public required SyntaxNode At;
            public required ISymbol Symbol;
            public required ITypeSymbol Type;
            public required EqualsValueClauseSyntax? Initializer;
            public required bool bReadOnly;
            public string SetKeyword = "set";
        }

        private List<FStoredMember> CollectStoredMembers(ClassDeclarationSyntax Node, bool bNativeBacked)
        {
            var Result = new List<FStoredMember>();
            foreach (MemberDeclarationSyntax Member in Node.Members)
            {
                if (Member is FieldDeclarationSyntax Field && MayNeedNativeStorage(Field.AttributeLists) && IsStored(Field, bNativeBacked))
                {
                    foreach (VariableDeclaratorSyntax Declarator in Field.Declaration.Variables)
                    {
                        if (Model.GetDeclaredSymbol(Declarator) is IFieldSymbol Symbol)
                        {
                            Result.Add(new FStoredMember
                            {
                                Declaration = Field,
                                At = Declarator,
                                Symbol = Symbol,
                                Type = Symbol.Type,
                                Initializer = Declarator.Initializer,
                                bReadOnly = Symbol.IsReadOnly,
                            });
                        }
                    }
                }
                else if (Member is PropertyDeclarationSyntax Property && bNativeBacked && MayNeedNativeStorage(Property.AttributeLists)
                         && AsStoredProperty(Property) is { } StoredProperty)
                {
                    Result.Add(StoredProperty);
                }
            }
            return Result;
        }

        // Native minted storage for this property, so its accessors must reach it, or the inspector, saves and prefabs edit a value the script never reads.
        private FStoredMember? AsStoredProperty(PropertyDeclarationSyntax Property)
        {
            if (Model.GetDeclaredSymbol(Property) is not IPropertySymbol Symbol || Symbol.IsStatic
                || Property.Modifiers.Any(SyntaxKind.PartialKeyword))
            {
                return null;
            }

            FScriptPropertyClassification Shape = ScriptPropertyClassifier.ClassifyMember(Symbol, Symbol.Type);
            bool bNativeFilled = Shape.IsView || Shape.Access == EScriptAccess.ComponentRef;
            EScriptMemberKind Kind = Symbol.SetMethod != null || bNativeFilled ? EScriptMemberKind.WritableProperty : EScriptMemberKind.ReadOnlyProperty;
            if (!ScriptPropertyClassifier.RuleFor(Symbol, Kind, bOwnerIsNativeObject: true).bStored)
            {
                return null;
            }

            string Where = $"{FilePath}({Line(Property)}): '{Symbol.Name}'";
            if (Symbol.IsAbstract)
            {
                Errors.Add($"{Where} is abstract, and a stored property needs storage of its own. Declare it on the class that holds the value.");
                return null;
            }
            if (Property.AccessorList == null
                || Property.AccessorList.Accessors.Any(Accessor => Accessor.Body != null || Accessor.ExpressionBody != null))
            {
                Errors.Add($"{Where} has accessor bodies, but the engine keeps a stored property's value in native memory, so a body would "
                         + "read and write a copy the inspector, saves and prefabs never see. Declare it as '{ get; set; }' or as a field.");
                return null;
            }
            if (Property.AccessorList.Accessors.Any(Accessor => Accessor.IsKind(SyntaxKind.InitAccessorDeclaration)))
            {
                Errors.Add($"{Where} has an init accessor, but the engine assigns a stored value after construction. "
                         + "Use 'private set' to keep it read-only to callers.");
                return null;
            }

            AccessorDeclarationSyntax? Setter = Property.AccessorList.Accessors
                .FirstOrDefault(Accessor => Accessor.IsKind(SyntaxKind.SetAccessorDeclaration));
            return new FStoredMember
            {
                Declaration = Property,
                At = Property,
                Symbol = Symbol,
                Type = Symbol.Type,
                Initializer = Property.Initializer,
                bReadOnly = Setter == null,
                SetKeyword = Setter != null && Setter.Modifiers.Count > 0 ? Setter.Modifiers.ToString() + " set" : "set",
            };
        }

        private SyntaxNode RewriteClass(ClassDeclarationSyntax Node, List<FStoredMember> Stored, List<FRpcMethod> Rpcs)
        {

            // typeof rather than a spelled name, so the runtime identity rule names it and a nested class resolves too.
            string TypeName = Model.GetDeclaredSymbol(Node) is { } Declared
                ? Declared.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)
                : Node.Identifier.Text;

            // Keyed by declaration, since `float A, B;` declares several members and each expands to statics plus a property.
            var Replacements = new Dictionary<MemberDeclarationSyntax, List<MemberDeclarationSyntax>>();
            var Defaults = new List<string>();

            foreach (FStoredMember Member in Stored)
            {
                string Name = Member.Symbol.Name;

                // The shared classifier also runs in the IDE analyzer, so what fails here was already underlined there.
                FScriptPropertyClassification Classification = ScriptPropertyClassifier.ClassifyMember(Member.Symbol, Member.Type);
                if (!Classification.IsSupported)
                {
                    Errors.Add($"{FilePath}({Line(Member.At)}): [Property] '{Name}' has type "
                             + $"'{Member.Type.ToDisplayString()}': {Classification.Rejection}");
                    continue;
                }

                if (Classification.Access == EScriptAccess.ComponentRef && Member.Initializer != null)
                {
                    Errors.Add($"{FilePath}({Line(Member.At)}): [Property(Component)] '{Name}' is bound to the entity's component before OnAttach, so it cannot be initialized.");
                    continue;
                }

                // A container is a view over native storage, so it is filled rather than assigned.
                if (Classification.IsView && Member.Initializer != null)
                {
                    Errors.Add($"{FilePath}({Line(Member.At)}): [Property] '{Name}' is a container. It is a "
                             + "view over the native storage, so it cannot be initialized; add to it in OnReady instead.");
                    continue;
                }

                // Rewriting it would hand DispatchAction a string instead of the live binding.
                if (Classification.KeepsManagedField)
                {
                    continue;
                }

                if (Member.Initializer != null)
                {
                    Defaults.Add($"{Name} = {Member.Initializer.Value};");
                }

                (CollectionsMarshal.GetValueRefOrAddDefault(Replacements, Member.Declaration, out _) ??= new()).AddRange(BuildProperty(Member, TypeName, Classification));
            }

            if (Errors.Count > 0)
            {
                return Node;
            }

            // Each field is replaced in place by its property, so the inspector's rows keep the source order.
            var RpcBodies = new Dictionary<MethodDeclarationSyntax, MethodDeclarationSyntax>();
            foreach (FRpcMethod Rpc in Rpcs)
            {
                RpcBodies[Rpc.Syntax] = RouteRpc(Rpc);
            }

            var Members = new List<MemberDeclarationSyntax>();
            foreach (MemberDeclarationSyntax Member in Node.Members)
            {
                if (Replacements.TryGetValue(Member, out List<MemberDeclarationSyntax>? Built))
                {
                    Members.AddRange(Built);
                    continue;
                }
                if (Member is MethodDeclarationSyntax Method && RpcBodies.TryGetValue(Method, out MethodDeclarationSyntax? Routed))
                {
                    Members.Add(Routed);
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

        private sealed class FRpcMethod
        {
            public required MethodDeclarationSyntax Syntax;
            public required IMethodSymbol Symbol;
        }

        private List<FRpcMethod> CollectRpcs(ClassDeclarationSyntax Node)
        {
            var Result = new List<FRpcMethod>();
            var SeenNames = new HashSet<string>();
            foreach (MethodDeclarationSyntax Method in Node.Members.OfType<MethodDeclarationSyntax>().Where(MayBeRpc))
            {
                if (Model.GetDeclaredSymbol(Method) is not IMethodSymbol Symbol)
                {
                    continue;
                }

                AttributeData? Attribute = Symbol.GetAttributes().FirstOrDefault(Data =>
                    Data.AttributeClass?.ContainingType?.Name == "Rpc"
                    && Data.AttributeClass.Name is "BroadcastAttribute" or "HostAttribute" or "OwnerAttribute");
                if (Attribute == null)
                {
                    continue;
                }

                string Where = $"{FilePath}({Line(Method)}): [Rpc] '{Symbol.Name}'";
                if (!Symbol.ReturnsVoid || Symbol.IsStatic || Symbol.IsGenericMethod || Symbol.IsAsync)
                {
                    Errors.Add($"{Where} must be a non-static, non-generic, non-async method returning void, since the call returns before it runs remotely.");
                    continue;
                }
                if (Symbol.Parameters.Any(Parameter => Parameter.RefKind != RefKind.None))
                {
                    Errors.Add($"{Where} has a ref or out parameter, which a remote call has no way to hand back.");
                    continue;
                }
                if (!DerivesFrom(Symbol.ContainingType, EntityScriptSymbol))
                {
                    Errors.Add($"{Where} is declared on {Symbol.ContainingType.Name}, and only an CEntityScript has an entity to route the call through.");
                    continue;
                }
                if (!SeenNames.Add(Symbol.Name))
                {
                    Errors.Add($"{Where} is overloaded, and a remote call is matched by name alone.");
                    continue;
                }
                if (Method.Body == null && Method.ExpressionBody == null)
                {
                    Errors.Add($"{Where} has no body to route.");
                    continue;
                }

                Result.Add(new FRpcMethod
                {
                    Syntax = Method,
                    Symbol = Symbol,
                });
            }
            return Result;
        }

        // Every declarator of one field shares its attributes, so the first one answers for all of them.
        private bool IsStored(FieldDeclarationSyntax Field, bool bNativeBacked)
        {
            VariableDeclaratorSyntax? First = Field.Declaration.Variables.FirstOrDefault();
            return First != null && Model.GetDeclaredSymbol(First) is IFieldSymbol Symbol
                && ScriptPropertyClassifier.RuleFor(Symbol, EScriptMemberKind.Field, bNativeBacked).bStored;
        }

        private static bool DerivesFrom(INamedTypeSymbol? Type, INamedTypeSymbol? Base)
        {
            for (INamedTypeSymbol? Current = Type; Current != null && Base != null; Current = Current.BaseType)
            {
                if (SymbolEqualityComparer.Default.Equals(Current, Base))
                {
                    return true;
                }
            }
            return false;
        }

        private static string Qualify(ITypeSymbol Type) => Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);

        // One line on the body's first line, so no later line number moves. Native serializes the frame as it does a C++ NET_RPC.
        private MethodDeclarationSyntax RouteRpc(FRpcMethod Rpc)
        {
            var Prefix = new StringBuilder();
            Prefix.Append("if (global::LuminaSharp.RpcRuntime.Begin(this, \"").Append(Rpc.Symbol.Name).Append("\", out var __rpc)) { ");
            foreach (IParameterSymbol Parameter in Rpc.Symbol.Parameters)
            {
                Prefix.Append("__rpc.Add<").Append(Qualify(Parameter.Type)).Append(">(").Append(Parameter.Name).Append("); ");
            }
            Prefix.Append("if (__rpc.Send()) { return; } } ");

            StatementSyntax Gate = SyntaxFactory.ParseStatement(Prefix.ToString());
            MethodDeclarationSyntax Method = Rpc.Syntax;

            if (Method.Body != null)
            {
                return Method.WithBody(Method.Body.WithStatements(Method.Body.Statements.Insert(0, Gate)));
            }

            // An expression body becomes a block on the same line, keeping the line count.
            ArrowExpressionClauseSyntax Arrow = Method.ExpressionBody!;
            BlockSyntax Block = SyntaxFactory.Block(Gate, SyntaxFactory.ExpressionStatement(Arrow.Expression))
                .WithLeadingTrivia(Arrow.GetLeadingTrivia())
                .WithTrailingTrivia(Method.SemicolonToken.TrailingTrivia);
            return Method.WithExpressionBody(null).WithSemicolonToken(default).WithBody(Block);
        }



        // The property replacing one field, wrapped in #line directives so traces and breakpoints name the line the author wrote.
        private IEnumerable<MemberDeclarationSyntax> BuildProperty(FStoredMember Member, string TypeName, FScriptPropertyClassification Classification)
        {
            string Name = Member.Symbol.Name;
            SyntaxNode Declarator = Member.At;
            string Type = Member.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
            string AnnotatedType = Member.Type.ToDisplayString(NullableQualifiedFormat);
            string Offset = $"__lazyoff_{Name}.Get(typeof({TypeName}), \"{Name}\")";
            string Token = $"__lazyprop_{Name}.Get(typeof({TypeName}), \"{Name}\")";
            bool bWritable = !Classification.IsView && !Member.bReadOnly;

            string Get;
            string? Set = null;
            string Unbound = "default";

            switch (Classification.Access)
            {
                case EScriptAccess.Blittable:
                    Get = $"global::System.Runtime.CompilerServices.Unsafe.ReadUnaligned<{Type}>((void*)((nint){SelfHandle} + {Offset}))";
                    Set = $"global::System.Runtime.CompilerServices.Unsafe.WriteUnaligned((void*)((nint){SelfHandle} + {Offset}), value)";
                    break;

                // The minted enum property is its underlying type's width, so the enum is read in place.
                case EScriptAccess.Enum:
                    Get = $"global::System.Runtime.CompilerServices.Unsafe.ReadUnaligned<{Type}>((void*)((nint){SelfHandle} + {Offset}))";
                    Set = $"global::System.Runtime.CompilerServices.Unsafe.WriteUnaligned((void*)((nint){SelfHandle} + {Offset}), value)";
                    break;

                // A string member makes the native value a different shape from the managed one, so it crosses field by field.
                case EScriptAccess.MarshalledStruct:
                    Get = $"global::LuminaSharp.ScriptStructMarshal<{Type}>.Read((nint){SelfHandle} + {Offset})";
                    Set = $"global::LuminaSharp.ScriptStructMarshal<{Type}>.Write((nint){SelfHandle} + {Offset}, value)";
                    break;

                case EScriptAccess.String:
                    Get = $"global::LuminaSharp.NativeMarshal.ReadString((nint){SelfHandle} + {Offset})";
                    Set = $"global::LuminaSharp.Native.StringAssign((nint){SelfHandle} + {Offset}, value)";
                    Unbound = "\"\"";
                    break;

                // Every asset reference is stored natively as an FSoftObjectPath, routed by its interface so a new reference type needs nothing here.
                case EScriptAccess.AssetPath:
                    Get = $"global::LuminaSharp.SoftObjectReferenceMarshal.Read<{Type}>("
                        + $"global::LuminaSharp.Native.SoftPathGet((nint){SelfHandle} + {Offset}))";
                    Set = $"global::LuminaSharp.Native.SoftPathSet((nint){SelfHandle} + {Offset}, "
                        + $"global::LuminaSharp.SoftObjectReferenceMarshal.Write(value))";
                    break;

                // The canonical wrapper through NativeObjectMarshal, which the user's assembly can reach where Wrapper<T> is internal.
                case EScriptAccess.Object:
                    Get = $"global::LuminaSharp.NativeObjectMarshal.FromHandle<{Type}>("
                        + $"global::LuminaSharp.Native.GetObjectPtr((nint){SelfHandle} + {Offset}))";
                    Set = $"global::LuminaSharp.Native.SetObjectPtr((nint){SelfHandle} + {Offset}, "
                        + "global::LuminaSharp.NativeObjectMarshal.ToHandle(value))";
                    break;

                // A handle on the embedded struct, so edits through it land in the owner's storage.
                case EScriptAccess.NativeStruct:
                    Get = $"global::LuminaSharp.NativeStructs.FromAddress<{Type}>((nint){SelfHandle} + {Offset})";
                    Set = $"global::LuminaSharp.Native.PropCopyStruct({SelfHandle}, {Token}, "
                        + "global::LuminaSharp.NativeStructs.AddressOf(value))";
                    break;

                // Native binds the reference before OnAttach, and it is read as the TComponentRef it was laid out to be.
                case EScriptAccess.ComponentRef:
                    Get = $"*({Type}*)((nint){SelfHandle} + {Offset})";
                    break;

                // A strong object reference, stored natively as an object property so it keeps its target alive.
                case EScriptAccess.ObjectPtr:
                    Get = $"new {Type}(global::LuminaSharp.Native.GetObjectPtr((nint){SelfHandle} + {Offset}))";
                    Set = $"global::LuminaSharp.Native.SetObjectPtr((nint){SelfHandle} + {Offset}, value.NativeHandle)";
                    break;

                case EScriptAccess.MapView:
                {
                    string View = $"global::Lumina.THashMap<{Qualified(Classification.Key!)}, "
                                + $"{Qualified(Classification.Value!)}>";
                    Type = View;
                    Get = $"new {View}((nint){SelfHandle} + {Offset}, (nint)global::LuminaSharp.Native.PropMapOps({Token}))";
                    break;
                }

                // One view for every element kind, since TVector routes each slot's read and write through ElementMarshal.
                case EScriptAccess.ListView:
                {
                    string View = $"global::Lumina.TVector<{Qualified(Classification.Element!)}>";
                    Type = View;
                    Get = $"new {View}((nint){SelfHandle} + {Offset}, (nint)global::LuminaSharp.Native.PropVectorOps({Token}))";
                    break;
                }

                case EScriptAccess.InstancedStruct:
                    Get = $"new {Type}((nint){SelfHandle} + {Offset})";
                    break;

                // Listed rather than defaulted, so a kind the classifier gains without this emitter learning it fails loudly.
                default:
                    Errors.Add($"{FilePath}({Line(Declarator)}): [Property] '{Name}' classified as "
                             + $"{Classification.Access}, which this emitter does not handle. This is an engine bug.");
                    return System.Array.Empty<MemberDeclarationSyntax>();
            }

            // Built first, so only the cache the accessors actually reached for gets emitted below.
            var Body = new StringBuilder();

            // A ref to a cached handle on a class owner, since a setter called on a handle returned by value is a compile error.
            if (Classification.Access == EScriptAccess.NativeStruct && !bStructOwner)
            {
                return BuildStructViewProperty(Member, Type, Offset);
            }
            // A component's value field reads as a ref into its storage, so ref, out and compound writes behave as the field the author declared.
            if (bComponentOwner && Classification.Access is EScriptAccess.Blittable or EScriptAccess.Enum)
            {
                return BuildComponentFieldRef(Member, Type, Offset);
            }
            AppendCarriedAttributes(Body, Member);
            string DeclaredType = Classification.Access is EScriptAccess.Object or EScriptAccess.AssetPath or EScriptAccess.String
                ? AnnotatedType
                : Type;
            Body.Append(MemberModifiers(Member.Declaration.Modifiers)).Append(' ').Append(DeclaredType).Append(' ').AppendLine(Name);
            Body.AppendLine("{");
            // Gated, since the schema pass describes each type through an unbound instance whose handle is null, which a component never is.
            if (bComponentOwner)
            {
                Body.Append("    get => ").Append(Get).AppendLine(";");
            }
            else
            {
                Body.Append("    get => HasNativeStorage ? ").Append(Get).Append(" : ").Append(Unbound).AppendLine(";");
            }
            bool bSync = HasSpecifier(Member.Symbol, ScriptMemberRules.Sync);
            if (bSync && Classification.IsView)
            {
                Errors.Add($"{FilePath}({Line(Declarator)}): [Property(Sync)] '{Name}' is a container, and only whole values replicate. "
                         + "Sync a count or an id instead, or send the contents through an RPC.");
                return System.Array.Empty<MemberDeclarationSyntax>();
            }
            if (bWritable && Set != null && bSync)
            {
                if (!BuildSyncSetter(Body, Member, Type, Get, Set))
                {
                    return System.Array.Empty<MemberDeclarationSyntax>();
                }
            }
            else if (bWritable && Set != null && bComponentOwner)
            {
                Body.Append("    ").Append(Member.SetKeyword).Append(" => ").Append(Set).AppendLine(";");
            }
            else if (bWritable && Set != null)
            {
                Body.Append("    ").Append(Member.SetKeyword).Append(" { if (HasNativeStorage) { ").Append(Set).AppendLine("; } }");
            }
            Body.AppendLine("}");

            string BodyText = Body.ToString();

            var Builder = new StringBuilder();
            if (BodyText.Contains("__lazyoff_" + Name))
            {
                Builder.Append("private static global::LuminaSharp.LazyPropertyOffset __lazyoff_").Append(Name).AppendLine(";");
            }
            if (BodyText.Contains("__lazyprop_" + Name))
            {
                Builder.Append("private static global::LuminaSharp.LazyPropertyToken __lazyprop_").Append(Name).AppendLine(";");
            }
            Builder.Append(BodyText);
            return MapToAuthoredLines(Builder.ToString(), Member);
        }

        // The attributes carry over verbatim, since TypeLibrary finds the member and its inspector metadata through them.
        private static void AppendCarriedAttributes(StringBuilder Builder, FStoredMember Member)
        {
            foreach (AttributeListSyntax List in Member.Declaration.AttributeLists)
            {
                // A field-targeted attribute on an auto-property has no backing field left to land on.
                if (Member.Declaration is PropertyDeclarationSyntax && List.Target?.Identifier.IsKind(SyntaxKind.FieldKeyword) == true)
                {
                    continue;
                }
                Builder.AppendLine(List.ToString());
            }
        }

        // Parsed as a class body, since ParseMemberDeclaration returns only the first member and dropped the property.
        private List<MemberDeclarationSyntax> MapToAuthoredLines(string Source, FStoredMember Member)
        {
            List<MemberDeclarationSyntax> Members = ParseMembers(Source).ToList();
            if (Members.Count == 0)
            {
                return Members;
            }

            // Attached as trivia so they survive extraction, the first mapping the expansion to the field and the second restoring the real numbering after it.
            const int OneBased = 1;
            const int NextLine = 1;
            Members[0] = Members[0].WithLeadingTrivia(Directive(Line(Member.At)));
            Members[^1] = Members[^1].WithTrailingTrivia(
                Members[^1].GetTrailingTrivia().AddRange(
                    Directive(Member.Declaration.GetLocation().GetLineSpan().EndLinePosition.Line + OneBased + NextLine)));
            return Members;
        }

        private IEnumerable<MemberDeclarationSyntax> BuildComponentFieldRef(FStoredMember Member, string Type, string Offset)
        {
            string Name = Member.Symbol.Name;
            var Builder = new StringBuilder();
            Builder.Append("private static global::LuminaSharp.LazyPropertyOffset __lazyoff_").Append(Name).AppendLine(";");
            AppendCarriedAttributes(Builder, Member);
            Builder.Append(MemberModifiers(Member.Declaration.Modifiers)).Append(Member.bReadOnly ? " ref readonly " : " ref ").Append(Type).Append(' ').Append(Name)
                   .Append(" => ref global::System.Runtime.CompilerServices.Unsafe.AsRef<").Append(Type).Append(">((void*)((nint)").Append(SelfHandle)
                   .Append(" + ").Append(Offset).AppendLine("));");
            return MapToAuthoredLines(Builder.ToString(), Member);
        }

        private IEnumerable<MemberDeclarationSyntax> BuildStructViewProperty(FStoredMember Member, string Type, string Offset)
        {
            string Name = Member.Symbol.Name;
            var Builder = new StringBuilder();
            Builder.Append("private static global::LuminaSharp.LazyPropertyOffset __lazyoff_").Append(Name).AppendLine(";");
            Builder.Append("private ").Append(Type).Append(" __view_").Append(Name).AppendLine(";");
            foreach (AttributeListSyntax List in Member.Declaration.AttributeLists)
            {
                Builder.AppendLine(List.ToString());
            }
            Builder.Append(Accessibility(Member.Declaration.Modifiers)).Append(" ref readonly ").Append(Type).Append(' ').AppendLine(Name);
            Builder.AppendLine("{");
            Builder.Append("    get { __view_").Append(Name).Append(" = HasNativeStorage ? global::LuminaSharp.NativeStructs.FromAddress<").Append(Type)
                .Append(">((nint)").Append(SelfHandle).Append(" + ").Append(Offset).Append(") : default; return ref __view_").Append(Name).AppendLine("; }");
            Builder.AppendLine("}");

            List<MemberDeclarationSyntax> Members = ParseMembers(Builder.ToString()).ToList();
            Members[0] = Members[0].WithLeadingTrivia(Directive(Line(Member.At)));
            Members[^1] = Members[^1].WithTrailingTrivia(Members[^1].GetTrailingTrivia().AddRange(
                Directive(Member.Declaration.GetLocation().GetLineSpan().EndLinePosition.Line + 2)));
            return Members;
        }

        private static bool IsNumber(ITypeSymbol Type) => Type.SpecialType is SpecialType.System_Single or SpecialType.System_Double
            or SpecialType.System_Byte or SpecialType.System_SByte or SpecialType.System_Int16 or SpecialType.System_UInt16
            or SpecialType.System_Int32 or SpecialType.System_UInt32 or SpecialType.System_Int64 or SpecialType.System_UInt64;

        // Snaps, writes, then on a real change marks dirty, forwards an owner's write to the host and runs the change handler.
        private bool BuildSyncSetter(StringBuilder Body, FStoredMember Member, string Type, string Get, string Set)
        {
            ISymbol Symbol = Member.Symbol;
            string Where = $"{FilePath}({Line(Member.At)}): [Property(Sync)] '{Symbol.Name}'";
            AttributeData Property = ScriptPropertyClassifier.FindAttribute(Symbol, ScriptPropertyTypeNames.PropertyAttribute)!;

            // An enum constant arrives as its underlying integer, whatever width the native enum chose.
            uint Flags = ScriptPropertyClassifier.NamedArgument(Property, "Sync") is System.IConvertible Raw ? System.Convert.ToUInt32(Raw) : 0u;
            bool bFromOwner = (Flags & 1u) != 0;
            double Quantize = ScriptPropertyClassifier.NamedArgument(Property, "Quantize") is double Snap ? Snap : 0.0;
            string? Validate = ScriptPropertyClassifier.NamedArgument(Property, "Validate") as string;
            string? Change = ScriptPropertyClassifier.NamedArgument(Property, "Change") as string;

            if (Quantize > 0.0 && !IsNumber(Member.Type))
            {
                Errors.Add($"{Where} sets Quantize, which only a number can snap to.");
                return false;
            }
            if (bFromOwner && !DerivesFrom(Symbol.ContainingType, EntityScriptSymbol))
            {
                Errors.Add($"{Where} is FromOwner, and only an CEntityScript has an owner to take the write from.");
                return false;
            }
            if (Validate != null && !bFromOwner)
            {
                Errors.Add($"{Where} names Validate without Sync = ESyncFlags.FromOwner, and only an owner's write is validated.");
                return false;
            }
            if (Validate != null && !HasValidator(Symbol.ContainingType, Validate, Member.Type))
            {
                Errors.Add($"{Where} names Validate method '{Validate}', which must take one {Member.Type.ToDisplayString()} and return bool.");
                return false;
            }

            string Step = Quantize.ToString("R", System.Globalization.CultureInfo.InvariantCulture) + "d";
            Body.Append("    ").Append(Member.SetKeyword).Append(" { if (HasNativeStorage) { ");
            if (Quantize > 0.0)
            {
                Body.Append("value = (").Append(Type).Append(")(global::System.Math.Round((double)value / ").Append(Step).Append(") * ").Append(Step).Append("); ");
            }
            Body.Append("var __old = ").Append(Get).Append("; ").Append(Set)
                .Append("; if (!global::System.Collections.Generic.EqualityComparer<").Append(Type)
                .Append(">.Default.Equals(__old, value)) { global::LuminaSharp.SyncRuntime.MarkDirty(this); ");

            // Native sends the stored value through the field's FProperty, as it does for a C++ PROPERTY(Sync = FromOwner).
            if (bFromOwner)
            {
                Body.Append("if (global::LuminaSharp.SyncRuntime.ShouldForward(this)) { global::LuminaSharp.SyncRuntime.Forward(this, \"")
                    .Append(Symbol.Name).Append("\"); } ");
            }

            if (Change != null)
            {
                Body.Append("global::LuminaSharp.SyncRuntime.Changed(this, \"").Append(Symbol.Name).Append("\", __old, value); ");
            }

            Body.AppendLine("} } }");
            return true;
        }

        private static bool HasValidator(INamedTypeSymbol Type, string Name, ITypeSymbol ValueType)
        {
            for (INamedTypeSymbol? Current = Type; Current != null; Current = Current.BaseType)
            {
                foreach (IMethodSymbol Method in Current.GetMembers(Name).OfType<IMethodSymbol>())
                {
                    if (Method.Parameters.Length == 1 && Method.ReturnType.SpecialType == SpecialType.System_Boolean
                        && SymbolEqualityComparer.Default.Equals(Method.Parameters[0].Type, ValueType))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        private SyntaxTriviaList Directive(int SourceLine)
        {
            return SyntaxFactory.ParseLeadingTrivia($"#line {SourceLine} \"{FilePath.Replace("\\", "\\\\")}\"\n");
        }

        private static IEnumerable<MemberDeclarationSyntax> ParseMembers(string Text)
        {
            // The newlines matter, since a #line directive is only recognized at the start of a line.
            CompilationUnitSyntax Unit = SyntaxFactory.ParseCompilationUnit("class __Members__ {\n" + Text + "\n}");
            return Unit.Members.OfType<ClassDeclarationSyntax>().SelectMany(Class => Class.Members);
        }

        // The declared initializers replayed against the CDO, not a constructor that would overwrite authored values on load.
        private static IEnumerable<MemberDeclarationSyntax> BuildDefaultsMethod(List<string> Defaults)
        {
            var Builder = new StringBuilder();
            // The base virtual is plain protected (NativeObject.ApplyScriptDefaults wraps it for the engine).
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

        private static string Accessibility(SyntaxTokenList DeclaredModifiers)
        {
            // Unsafe because every accessor dereferences the container, the field's access carrying over to the property.
            List<string> Modifiers = DeclaredModifiers
                .Select(Token => Token.Text)
                .Where(Text => Text is "public" or "private" or "protected" or "internal" or "new" or "virtual" or "override" or "sealed")
                .ToList();
            if (!Modifiers.Any(Text => Text is "public" or "private" or "protected" or "internal"))
            {
                Modifiers.Add("private");
            }
            Modifiers.Add("unsafe");
            return string.Join(" ", Modifiers);
        }

        // A script struct's accessors write native memory rather than the handle, so they run on a readonly copy too.
        private string MemberModifiers(SyntaxTokenList DeclaredModifiers)
        {
            return bStructOwner ? Accessibility(DeclaredModifiers) + " readonly" : Accessibility(DeclaredModifiers);
        }

        // Fully qualified, so a view's element type resolves wherever the user's own usings point.
        private static string Qualified(ITypeSymbol Type) =>
            Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);

        private static readonly SymbolDisplayFormat NullableQualifiedFormat = SymbolDisplayFormat.FullyQualifiedFormat
            .AddMiscellaneousOptions(SymbolDisplayMiscellaneousOptions.IncludeNullableReferenceTypeModifier);
    }
}
