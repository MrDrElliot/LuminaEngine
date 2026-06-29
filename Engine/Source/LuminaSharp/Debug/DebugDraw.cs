using Lumina;

namespace LuminaSharp;

/// <summary>
/// A world's debug-draw interface (<c>World.Draw</c>). Dev/Debug builds only, the draws are no-ops in
/// Shipping. <c>Duration</c> &lt;= 0 draws for one frame.
/// Game thread only. Forwards to flat <c>LuminaSharp_Debug_*</c> shims in the Runtime module
/// (DotNetGameplay.cpp), with the world Handle passed first.
/// </summary>
public readonly unsafe partial struct DebugDraw
{
    internal readonly ulong Handle;

    internal DebugDraw(ulong Handle)
    {
        this.Handle = Handle;
    }

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Debug_DrawLine")]
    public partial void Line(FVector3 Start, FVector3 End, FVector4 Color, float Thickness = 1.0f, float Duration = 0.0f);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Debug_DrawSphere")]
    public partial void Sphere(FVector3 Center, float Radius, FVector4 Color, float Thickness = 1.0f, float Duration = 0.0f);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Debug_DrawBox")]
    public partial void Box(FVector3 Center, FVector3 HalfExtents, FQuat Rotation, FVector4 Color, float Thickness = 1.0f, float Duration = 0.0f);

    /// <summary>Screen-space debug text for this frame, stacked top-left on the world's viewport.</summary>
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Debug_DrawText")]
    public partial void Text(string Message, FVector4 Color);
}
