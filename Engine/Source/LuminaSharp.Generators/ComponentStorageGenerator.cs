using System.Linq;
using System.Threading;
using LuminaSharp.ScriptProperties;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Operations;

namespace LuminaSharp.Generators;

// A component only exists in its entity's storage, so any C# value of one would be a stray copy its accessors cannot read.
[Generator]
public sealed class ComponentStorageGenerator : IIncrementalGenerator
{
    private static readonly DiagnosticDescriptor HeldByValue = new(
        id: "LUM0120",
        title: "Component held by value",
        messageFormat: "'{0}' is a component, which lives in its entity's storage. Reach it through a ref, such as "
                     + "ref var Found = ref World.GetComponent<{0}>(Entity), or keep a TComponentRef<{0}>.",
        category: "LuminaSharp",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor CreatedAsValue = new(
        id: "LUM0121",
        title: "Component created as a value",
        messageFormat: "'{0}' is a component and only exists in storage. Add one with World.GetOrEmplaceComponent<{0}>(Entity).",
        category: "LuminaSharp",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public void Initialize(IncrementalGeneratorInitializationContext Context)
    {
        IncrementalValuesProvider<Diagnostic?> Diagnostics = Context.SyntaxProvider
            .CreateSyntaxProvider(
                static (Node, _) => IsCandidate(Node),
                static (Syntax, Cancel) => Validate(Syntax.Node, Syntax.SemanticModel, Cancel))
            .Where(static Item => Item != null);

        Context.RegisterSourceOutput(Diagnostics, static (Output, Item) => Output.ReportDiagnostic(Item!));
    }

    private static bool IsCandidate(SyntaxNode Node)
    {
        return Node is VariableDeclaratorSyntax or ParameterSyntax or ForEachStatementSyntax or SingleVariableDesignationSyntax
            or MethodDeclarationSyntax or LocalFunctionStatementSyntax or PropertyDeclarationSyntax or IndexerDeclarationSyntax
            or DelegateDeclarationSyntax or OperatorDeclarationSyntax or ConversionOperatorDeclarationSyntax
            or AnonymousFunctionExpressionSyntax
            or ObjectCreationExpressionSyntax or ImplicitObjectCreationExpressionSyntax or DefaultExpressionSyntax
            or WithExpressionSyntax or ArgumentSyntax or ArrayTypeSyntax or TupleTypeSyntax or TupleExpressionSyntax
            || Node.IsKind(SyntaxKind.DefaultLiteralExpression);
    }

    private static Diagnostic? Validate(SyntaxNode Node, SemanticModel Model, CancellationToken Cancel)
    {
        switch (Node)
        {
            case VariableDeclaratorSyntax Declarator:
                return Model.GetDeclaredSymbol(Declarator, Cancel) switch
                {
                    ILocalSymbol Local when !Local.IsRef => Held(Local.Type, Declarator.Identifier.GetLocation()),
                    IFieldSymbol Field when Field.RefKind == RefKind.None => Held(Field.Type, Declarator.Identifier.GetLocation()),
                    _ => null,
                };

            case ParameterSyntax Parameter:
                return Model.GetDeclaredSymbol(Parameter, Cancel) is IParameterSymbol { RefKind: RefKind.None } Symbol
                    ? Held(Symbol.Type, Parameter.GetLocation())
                    : null;

            case ForEachStatementSyntax ForEach:
                return Model.GetDeclaredSymbol(ForEach, Cancel) is ILocalSymbol { IsRef: false } Iterated
                    ? Held(Iterated.Type, ForEach.Identifier.GetLocation())
                    : null;

            case SingleVariableDesignationSyntax Designation:
                return Model.GetDeclaredSymbol(Designation, Cancel) is ILocalSymbol { IsRef: false } Designated
                    ? Held(Designated.Type, Designation.GetLocation())
                    : null;

            case MethodDeclarationSyntax or LocalFunctionStatementSyntax or PropertyDeclarationSyntax or IndexerDeclarationSyntax
                or DelegateDeclarationSyntax or OperatorDeclarationSyntax or ConversionOperatorDeclarationSyntax
                or AnonymousFunctionExpressionSyntax:
                return ValidateReturn(Node, Model, Cancel);

            case ObjectCreationExpressionSyntax or ImplicitObjectCreationExpressionSyntax or DefaultExpressionSyntax
                or WithExpressionSyntax:
            {
                ITypeSymbol? Created = Model.GetTypeInfo(Node, Cancel).Type;
                return Created != null && IsComponent(Created) ? Diagnostic.Create(CreatedAsValue, Node.GetLocation(), Created.Name) : null;
            }

            case ArgumentSyntax Argument:
            {
                // A by-value argument copies the component into the callee, a generic container's Add included.
                if (Model.GetOperation(Argument, Cancel) is not IArgumentOperation { Parameter.RefKind: RefKind.None } Passed)
                {
                    return null;
                }
                return Passed.Value is IConversionOperation { Operand.Type: { } Source } && IsComponent(Source)
                    ? Held(Source, Argument.GetLocation())
                    : Held(Passed.Value.Type, Argument.GetLocation());
            }

            case ArrayTypeSyntax Array:
                return Held(Model.GetTypeInfo(Array.ElementType, Cancel).Type, Array.GetLocation());

            case TupleTypeSyntax TupleType:
                return TupleType.Elements.Select(Element => Held(Model.GetTypeInfo(Element.Type, Cancel).Type, Element.GetLocation()))
                    .FirstOrDefault(Found => Found != null);

            case TupleExpressionSyntax Tuple:
                return Tuple.Arguments.Select(Element => Held(Model.GetTypeInfo(Element.Expression, Cancel).Type, Element.GetLocation()))
                    .FirstOrDefault(Found => Found != null);
        }

        if (Node.IsKind(SyntaxKind.DefaultLiteralExpression))
        {
            ITypeSymbol? Converted = Model.GetTypeInfo(Node, Cancel).ConvertedType;
            return Converted != null && IsComponent(Converted) ? Diagnostic.Create(CreatedAsValue, Node.GetLocation(), Converted.Name) : null;
        }
        return null;
    }

    private static Diagnostic? ValidateReturn(SyntaxNode Node, SemanticModel Model, CancellationToken Cancel)
    {
        ISymbol? Declared = Node is AnonymousFunctionExpressionSyntax
            ? Model.GetSymbolInfo(Node, Cancel).Symbol
            : Model.GetDeclaredSymbol(Node, Cancel);
        (ITypeSymbol? Returned, bool bByRef) = Declared switch
        {
            IMethodSymbol Method => (Method.ReturnType, Method.ReturnsByRef || Method.ReturnsByRefReadonly),
            IPropertySymbol Property => (Property.Type, Property.ReturnsByRef || Property.ReturnsByRefReadonly),
            INamedTypeSymbol { DelegateInvokeMethod: { } Invoke } => (Invoke.ReturnType, Invoke.ReturnsByRef || Invoke.ReturnsByRefReadonly),
            _ => (null, false),
        };
        if (bByRef || Returned == null)
        {
            return null;
        }
        Location Where = Node switch
        {
            MethodDeclarationSyntax Method => Method.ReturnType.GetLocation(),
            LocalFunctionStatementSyntax Local => Local.ReturnType.GetLocation(),
            PropertyDeclarationSyntax Property => Property.Type.GetLocation(),
            IndexerDeclarationSyntax Indexer => Indexer.Type.GetLocation(),
            DelegateDeclarationSyntax Delegate => Delegate.ReturnType.GetLocation(),
            OperatorDeclarationSyntax Operator => Operator.ReturnType.GetLocation(),
            ConversionOperatorDeclarationSyntax Conversion => Conversion.Type.GetLocation(),
            _ => Node.GetLocation(),
        };
        return Held(Returned, Where);
    }

    private static Diagnostic? Held(ITypeSymbol? Type, Location Where)
    {
        return Type != null && IsComponent(Type) ? Diagnostic.Create(HeldByValue, Where, Type.Name) : null;
    }

    // A component type, a nullable one, or a type parameter constrained to one, since each holds the component the same way.
    private static bool IsComponent(ITypeSymbol Type)
    {
        if (Type is INamedTypeSymbol { OriginalDefinition.SpecialType: SpecialType.System_Nullable_T } Nullable)
        {
            Type = Nullable.TypeArguments[0];
        }
        if (Type is ITypeParameterSymbol Parameter)
        {
            return Parameter.ConstraintTypes.Any(ScriptPropertyClassifier.IsComponentInterface);
        }
        return ScriptPropertyClassifier.IsComponent(Type);
    }
}
