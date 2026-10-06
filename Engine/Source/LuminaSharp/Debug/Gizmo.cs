using Lumina;

namespace LuminaSharp;

/// <summary>
/// Immediate-mode debug drawing for the current world (Dev/Debug only). State-driven like s&amp;box's Gizmo:
/// set <see cref="Color"/> / <see cref="Thickness"/> / <see cref="Duration"/> once, then draw. Forwards to CDebugDrawLibrary. Duration &lt;= 0 draws for one frame.
/// </summary>
public static class Gizmo
{
    public static Color Color = LuminaSharp.Color.White;
    public static float Thickness = 1.0f;
    public static float Duration = 0.0f;

    public static void Line(FVector3 Start, FVector3 End)
        => CDebugDrawLibrary.DrawLine(Engine.World, Start, End, Color, Thickness, Duration);

    public static void Line(FVector3 Start, FVector3 End, Color Color)
        => CDebugDrawLibrary.DrawLine(Engine.World, Start, End, Color, Thickness, Duration);

    public static void Sphere(FVector3 Center, float Radius)
        => CDebugDrawLibrary.DrawSphere(Engine.World, Center, Radius, Color, Thickness, Duration);

    public static void Box(FVector3 Center, FVector3 HalfExtents)
        => CDebugDrawLibrary.DrawBox(Engine.World, Center, HalfExtents, FQuat.Identity, Color, Thickness, Duration);

    public static void Box(FVector3 Center, FVector3 HalfExtents, FQuat Rotation)
        => CDebugDrawLibrary.DrawBox(Engine.World, Center, HalfExtents, Rotation, Color, Thickness, Duration);

    /// <summary>Screen-space text for this frame (stacked top-left).</summary>
    public static void Text(string Message)
        => CDebugDrawLibrary.DrawText(Engine.World, Message, Color);
}
