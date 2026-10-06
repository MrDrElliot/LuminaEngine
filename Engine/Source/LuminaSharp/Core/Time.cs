namespace LuminaSharp;

/// Frame timing for the current world; valid inside any gameplay callback.
public static class Time
{
    /// Seconds since the last frame.
    public static float Delta => Engine.World.DeltaTime;

    /// Alias of Delta.
    public static float DeltaTime => Engine.World.DeltaTime;

    /// Seconds since the world was created.
    public static double Now => Engine.World.ElapsedTime;
}
