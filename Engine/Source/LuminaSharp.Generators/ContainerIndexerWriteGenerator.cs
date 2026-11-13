using LuminaSharp.ScriptProperties;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace LuminaSharp.Generators;

// Rejects a write through TVector's indexer when the element is marshalled, since that indexer hands out a held copy.
[Generator]
public sealed class ContainerIndexerWriteGenerator : IIncrementalGenerator
{
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
                static (Node, _) => Node is ElementAccessExpressionSyntax Access && IsWriteTarget(WrittenExpression(Access)),
                static (Context, Cancel) => Validate((ElementAccessExpressionSyntax)Context.Node, Context.SemanticModel, Cancel))
            .Where(static Item => Item != null);

        context.RegisterSourceOutput(Diagnostics, static (Context, Item) => Context.ReportDiagnostic(Item!));
    }

    private static ExpressionSyntax SkipParentheses(ExpressionSyntax Inner)
    {
        while (Inner.Parent is ParenthesizedExpressionSyntax Parens)
        {
            Inner = Parens;
        }
        return Inner;
    }

    // The outermost member access rooted at the element, so List[0].A.B = x is judged by what it writes.
    private static ExpressionSyntax WrittenExpression(ElementAccessExpressionSyntax Access)
    {
        ExpressionSyntax Outer = SkipParentheses(Access);
        while (Outer.Parent is MemberAccessExpressionSyntax Member && Member.Expression == Outer)
        {
            Outer = SkipParentheses(Member);
        }
        return Outer;
    }

    private static bool IsWriteTarget(ExpressionSyntax Outer)
    {
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
        if (Owner.OriginalDefinition.ToDisplayString() != ScriptPropertyTypeNames.TVector || Owner.TypeArguments.Length != 1)
        {
            return null;
        }

        ITypeSymbol Element = Owner.TypeArguments[0];
        if (!IsMarshalled(Element))
        {
            return null;
        }

        ExpressionSyntax Written = WrittenExpression(Access);
        if (Written != SkipParentheses(Access) && !IsMemberWriteOnCopy(Access, Written, Element, Model, Cancel))
        {
            return null;
        }

        return Diagnostic.Create(MarshalledIndexerWrite, Access.GetLocation(), Element.ToDisplayString());
    }

    // A view element writes its members through a pointer, and a reference-typed member along the path writes into a shared object.
    private static bool IsMemberWriteOnCopy(ElementAccessExpressionSyntax Access, ExpressionSyntax Written, ITypeSymbol Element, SemanticModel Model, System.Threading.CancellationToken Cancel)
    {
        if (Element.TypeKind != TypeKind.Struct || Element.IsUnmanagedType || IsNativeView(Element))
        {
            return false;
        }
        for (SyntaxNode Node = Access.Parent!; Node != Written; Node = Node.Parent!)
        {
            if (Node is MemberAccessExpressionSyntax && Model.GetTypeInfo(Node, Cancel).Type?.IsValueType != true)
            {
                return false;
            }
        }
        return true;
    }

    // Mirrors ElementKinds.Of, where everything but a blittable value or a bool is marshalled.
    private static bool IsMarshalled(ITypeSymbol Type)
    {
        if (Type.TypeKind == TypeKind.TypeParameter || Type.SpecialType == SpecialType.System_Boolean)
        {
            return false;
        }
        // A script struct that needs marshalling holds a managed field, so the unmanaged check already covers it.
        if (!Type.IsUnmanagedType)
        {
            return true;
        }
        return Type is INamedTypeSymbol { IsGenericType: true } Generic
                && (Generic.OriginalDefinition.SpecialType == SpecialType.System_Nullable_T || Generic.ContainingNamespace?.ToDisplayString() == "Lumina")
            || IsNativeView(Type);
    }

    private static bool IsNativeView(ITypeSymbol Type)
    {
        return ScriptPropertyClassifier.FindAttribute(Type, ScriptPropertyTypeNames.NativeSlotViewAttribute) != null
            || ScriptPropertyClassifier.IsSoftObjectReference(Type)
            || ScriptPropertyClassifier.IsStructHandle(Type);
    }
}
