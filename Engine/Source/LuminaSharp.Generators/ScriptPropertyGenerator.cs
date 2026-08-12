using System.Linq;
using Microsoft.CodeAnalysis;

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
/// declaration, while you are typing it.
/// </summary>
[Generator]
public sealed class ScriptPropertyGenerator : IIncrementalGenerator
{
    private const string PropertyAttribute = "LuminaSharp.PropertyAttribute";

    private static readonly DiagnosticDescriptor UnsupportedType = new(
        id: "LUM0101",
        title: "Unsupported [Property] type",
        messageFormat: "[Property] '{0}' has type '{1}', which cannot be viewed over native storage. Supported: numbers, "
                     + "bool, enums, blittable struct mirrors, string, Lumina.FSoftObjectPath, NativeObject-derived "
                     + "references, and NativeList<T> of an unmanaged T.",
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
                static (Context, _) => Validate(Context.TargetSymbol, Context.TargetNode.GetLocation()));

        context.RegisterSourceOutput(Diagnostics, static (Context, Item) =>
        {
            if (Item != null)
            {
                Context.ReportDiagnostic(Item);
            }
        });
    }

    private static Diagnostic? Validate(ISymbol Symbol, Location Location)
    {
        // A partial property is the pre-rewriter shape. It still compiles in the IDE (nothing implements it
        // there either), so without this it would fail only at script reload, with CS9248 and no explanation.
        if (Symbol is IPropertySymbol Property)
        {
            return Diagnostic.Create(PartialProperty, Symbol.Locations.FirstOrDefault() ?? Location,
                Property.Name, Property.Type.ToDisplayString());
        }

        if (Symbol is not IFieldSymbol Field)
        {
            return null;
        }

        if (IsViewType(Field.Type, out ITypeSymbol? Element))
        {
            if (Element == null || !Element.IsUnmanagedType)
            {
                return Diagnostic.Create(UnsupportedType, Location, Field.Name, Field.Type.ToDisplayString());
            }
            // The initializer itself is checked by the rewriter, which can see it; a field symbol cannot.
            return null;
        }

        return IsSupported(Field.Type)
            ? null
            : Diagnostic.Create(UnsupportedType, Location, Field.Name, Field.Type.ToDisplayString());
    }

    private static bool IsViewType(ITypeSymbol Type, out ITypeSymbol? Element)
    {
        Element = null;
        if (Type is INamedTypeSymbol Named && Named.IsGenericType
            && Named.ConstructedFrom.ToDisplayString() == "LuminaSharp.NativeList<T>")
        {
            Element = Named.TypeArguments[0];
            return true;
        }
        return false;
    }

    // Kept in step with ScriptPropertyRewriter.Classify: this reports what that would refuse.
    private static bool IsSupported(ITypeSymbol Type)
    {
        if (Type.TypeKind == TypeKind.Enum
            || Type.SpecialType == SpecialType.System_String
            || Type.ToDisplayString() == "Lumina.FSoftObjectPath")
        {
            return true;
        }
        for (INamedTypeSymbol? Base = Type.BaseType; Base != null; Base = Base.BaseType)
        {
            if (Base.ToDisplayString() == "LuminaSharp.NativeObject")
            {
                return true;
            }
        }
        return Type.IsUnmanagedType;
    }
}
