using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Text;

namespace LuminaSharp.Generators;

// A cached model cannot hold a SyntaxTree, yet only a location in the current tree honors #pragma and SuppressMessage.
internal sealed record SourceSpot(string Path, TextSpan Span)
{
    public static SourceSpot Of(ISymbol Symbol) => new(Symbol.Locations[0].SourceTree!.FilePath, Symbol.Locations[0].SourceSpan);

    public Location ToLocation(Compilation Compilation) => Location.Create(Compilation.SyntaxTrees.First(Tree => Tree.FilePath == Path), Span);
}
