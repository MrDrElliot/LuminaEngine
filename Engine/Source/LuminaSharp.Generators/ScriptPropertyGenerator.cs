using System.Linq;
using LuminaSharp.ScriptProperties;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace LuminaSharp.Generators;

/// <summary>
/// Validates <c>[Property]</c> members. It emits nothing.
///
/// The accessors themselves are produced by <c>LuminaSharp.ScriptPropertyRewriter</c>, inside the engine's own
/// Roslyn compilation, because a script property's value lives in NATIVE memory and a field cannot be a view
/// over that -- only an accessor can. A source generator may only ADD members, never replace one, which is
/// what used to force authors to write <c>public partial float Speed { get; set; }</c> and left nowhere to put
/// a default (a partial property cannot have an initializer). The engine compiles scripts itself, so it
/// rewrites the field into that property instead, and the author writes ordinary C#.
///
/// This analyzer still exists because the rewriter runs at RUN TIME: without it an unsupported member would
/// look fine in the IDE and fail only on reload. Its job is to say the same thing the rewriter would, at the
/// declaration, while you are typing it -- so it does not decide anything itself. Every judgement here comes
/// from <see cref="ScriptPropertyClassifier"/>, the single copy the rewriter also runs on.
/// </summary>
[Generator]
public sealed class ScriptPropertyGenerator : IIncrementalGenerator
{
    private const string PropertyAttribute = "LuminaSharp.PropertyAttribute";

    private static readonly DiagnosticDescriptor UnsupportedType = new(
        id: "LUM0101",
        title: "Unsupported [Property] type",
        messageFormat: "[Property] '{0}' has type '{1}': {2}",
        category: "LuminaSharp",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor ContainerInitialized = new(
        id: "LUM0102",
        title: "Container [Property] cannot be initialized",
        messageFormat: "[Property] '{0}' is a container, so it is a VIEW over storage native already owns and cannot be "
                     + "assigned. Drop the initializer and fill it in OnReady (Add/Clear/RemoveAt).",
        category: "LuminaSharp",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor PartialProperty = new(
        id: "LUM0103",
        title: "[Property] should be a field",
        messageFormat: "[Property] '{0}' is a partial property. That was the old shape, needed back when a source "
                     + "generator produced the accessors; the engine now rewrites a plain field into the same thing. "
                     + "Declare it as 'public {1} {0};' -- which also gives an initializer somewhere to live, and that "
                     + "is what a default is.",
        category: "LuminaSharp",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        IncrementalValuesProvider<Diagnostic?> Diagnostics = context.SyntaxProvider
            .ForAttributeWithMetadataName(
                PropertyAttribute,
                static (_, _) => true,
                static (Context, _) => Validate(Context.TargetSymbol, Context.TargetNode));

        context.RegisterSourceOutput(Diagnostics, static (Context, Item) =>
        {
            if (Item != null)
            {
                Context.ReportDiagnostic(Item);
            }
        });
    }

    private static Diagnostic? Validate(ISymbol Symbol, SyntaxNode Node)
    {
        Location Location = Node.GetLocation();

        // A partial property is the pre-rewriter shape. It still compiles in the IDE (nothing implements it
        // there either), so without this it would fail only at script reload, with CS9248 and no explanation.
        if (Symbol is IPropertySymbol Property)
        {
            // The rewriter re-emits [Property] onto the property it generates, and this runs over that.
            if (!IsPartial(Property))
            {
                return null;
            }
            return Diagnostic.Create(PartialProperty, Symbol.Locations.FirstOrDefault() ?? Location,
                Property.Name, Property.Type.ToDisplayString());
        }

        if (Symbol is not IFieldSymbol Field)
        {
            return null;
        }

        FScriptPropertyClassification Classification = ScriptPropertyClassifier.Classify(Field.Type);
        if (!Classification.IsSupported)
        {
            return Diagnostic.Create(UnsupportedType, Location, Field.Name, Field.Type.ToDisplayString(),
                Classification.Rejection);
        }

        // A container is a view over storage native owns, so assigning it is meaningless while its contents
        // are fully editable. The rewriter refuses this too; reported here so it surfaces at the declaration.
        if (Classification.IsView && Declarator(Field, Node)?.Initializer != null)
        {
            return Diagnostic.Create(ContainerInitialized, Location, Field.Name);
        }

        return null;
    }

    private static bool IsPartial(IPropertySymbol Property)
    {
        foreach (SyntaxReference Reference in Property.DeclaringSyntaxReferences)
        {
            if (Reference.GetSyntax() is PropertyDeclarationSyntax Declaration
                && Declaration.Modifiers.Any(SyntaxKind.PartialKeyword))
            {
                return true;
            }
        }
        return false;
    }

    /// <summary>The declarator for a field, which is where an initializer lives -- an <see cref="IFieldSymbol"/>
    /// cannot see one. Normally the attribute's own target node; falls back through the symbol for the shapes
    /// where it is not (several declarators sharing one <c>[Property]</c> field declaration).</summary>
    private static VariableDeclaratorSyntax? Declarator(IFieldSymbol Field, SyntaxNode Node)
    {
        if (Node is VariableDeclaratorSyntax Direct)
        {
            return Direct;
        }
        return Field.DeclaringSyntaxReferences
            .Select(Reference => Reference.GetSyntax())
            .OfType<VariableDeclaratorSyntax>()
            .FirstOrDefault();
    }
}
