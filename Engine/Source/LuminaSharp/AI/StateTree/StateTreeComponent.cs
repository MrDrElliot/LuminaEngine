using System;
using LuminaSharp;

namespace Lumina;

public readonly unsafe partial struct SStateTreeComponent
{
    public bool SendEvent(HierarchicalTag Tag) => SendEvent(Tag.Id);

    // The running instance's parameters, whose writes take effect on its next tick.
    public T GetParameters<T>() where T : struct, INativeStruct<T>
        => TryGetParameters<T>() ?? throw new InvalidOperationException($"State tree parameters are not a running {T.NativeName}.");

    public T? TryGetParameters<T>() where T : struct, INativeStruct<T>
        => NativeStructs.FromAddressOrNull<T>(GetParameterMemoryAs(T.NativeName));
}
