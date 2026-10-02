using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace LuminaSharp.Generators;

// Rejects a write through TVector's indexer when the element is marshalled, since that indexer hands out a held copy.
[Generator]
public sealed class ContainerIndexerWriteGenerator : IIncrementalGenerator
{
    private const string VectorType = "Lumina.TVector<T>";

    private static readonly DiagnosticDescriptor MarshalledIndexerWrite = new(
        id: "LUM0201",
        title: "Write through a marshalled TVector indexer",
        messageFormat: "TVector<{0}> reads through its indexer, but an element of that type is not its native bytes, "
                     + "so a write there would land on a copy. Use Set(index, value).",
        category: "LuminaSharp",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        IncrementalValuesProvider<Diagnostic?> Diagnostics = context.SyntaxProvider
            .CreateSyntaxProvider(
                static (Node, _) => Node is ElementAccessExpressionSyntax Access && IsWriteTarget(Access),
                static (Context, Cancel) => Validate((ElementAccessExpressionSyntax)Context.Node, Context.SemanticModel, Cancel))
            .Where(static Item => Item != null);

        context.RegisterSourceOutput(Diagnostics, static (Context, Item) => Context.ReportDiagnostic(Item!));
    }

    private static bool IsWriteTarget(ElementAccessExpressionSyntax Access)
    {
        ExpressionSyntax Outer = Access;
        while (Outer.Parent is ParenthesizedExpressionSyntax Parens)
        {
            Outer = Parens;
        }

        // A member write such as List[0].Field = x goes through a view and is left alone, so only the element itself counts.
        return Outer.Parent switch
        {
            AssignmentExpressionSyntax Assign => Assign.Left == Outer,
            PrefixUnaryExpressionSyntax Prefix => Prefix.IsKind(SyntaxKind.PreIncrementExpression) || Prefix.IsKind(SyntaxKind.PreDecrementExpression),
            PostfixUnaryExpressionSyntax Postfix => Postfix.IsKind(SyntaxKind.PostIncrementExpression) || Postfix.IsKind(SyntaxKind.PostDecrementExpression),
            RefExpressionSyntax => true,
            ArgumentSyntax Argument => Argument.RefKindKeyword.IsKind(SyntaxKind.RefKeyword)
                                    || Argument.RefKindKeyword.IsKind(SyntaxKind.OutKeyword)
                                    || IsDeconstructionTarget(Argument),
            _ => false,
        };
    }

    private static bool IsDeconstructionTarget(ArgumentSyntax Argument)
    {
        SyntaxNode Tuple = Argument.Parent!;
        while (Tuple.Parent is ArgumentSyntax { Parent: TupleExpressionSyntax Outer })
        {
            Tuple = Outer;
        }
        return Tuple is TupleExpressionSyntax && Tuple.Parent is AssignmentExpressionSyntax Assign && Assign.Left == Tuple;
    }

    private static Diagnostic? Validate(ElementAccessExpressionSyntax Access, SemanticModel Model, System.Threading.CancellationToken Cancel)
    {
        if (Model.GetSymbolInfo(Access, Cancel).Symbol is not IPropertySymbol { IsIndexer: true } Indexer)
        {
            return null;
        }

        INamedTypeSymbol Owner = Indexer.ContainingType;
        if (Owner.OriginalDefinition.ToDisplayString() != VectorType || Owner.TypeArguments.Length != 1)
        {
            return null;
        }

        ITypeSymbol Element = Owner.TypeArguments[0];
        if (!IsMarshalled(Element))
        {
            return null;
        }

        return Diagnostic.Create(MarshalledIndexerWrite, Access.GetLocation(), Element.ToDisplayString());
    }

    // Mirrors ElementKinds.Of, where everything but a blittable value or a bool is marshalled.
    private static bool IsMarshalled(ITypeSymbol Type)
    {
        if (Type.TypeKind == TypeKind.TypeParameter || Type.SpecialType == SpecialType.System_Boolean)
        {
            return false;
        }
        if (!Type.IsUnmanagedType)
        {
            return true;
        }
        if (Type is INamedTypeSymbol { IsGenericType: true } Generic)
        {
            return Generic.OriginalDefinition.SpecialType == SpecialType.System_Nullable_T
                || Generic.ContainingNamespace?.ToDisplayString() == "Lumina";
        }
        if (Type.GetAttributes().Any(Attribute => Attribute.AttributeClass?.Name == "NativeSlotViewAttribute"))
        {
            return true;
        }
        return Type.AllInterfaces.Any(Interface => Interface.Name == "ISoftObjectReference");
    }
}
