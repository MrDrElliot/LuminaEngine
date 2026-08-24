using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

/// <summary>
/// A world's camera control surface (<c>World.Camera</c>). Exposes additive camera shake on the active view:
/// positional + rotational oscillation that composes with the cinematic blend and never moves the camera
/// entity itself. Several shakes sum. Game thread only. Each member forwards to a flat
/// <c>LuminaSharp_Camera_*</c> shim in the Runtime module (DotNetGameplay.cpp), with the world Handle first.
/// </summary>
public readonly unsafe partial struct Camera
{
    internal readonly ulong Handle;

    internal Camera(ulong Handle)
    {
        this.Handle = Handle;
    }

    public bool IsValid => Handle != 0;

    // Read from the resolved view, so shake and any active blend are already folded into the basis.

    /// World-space position of the rendered view.
    public FVector3 Position => GetPositionRaw();

    /// Normalized forward axis of the rendered view.
    public FVector3 Forward => GetForwardRaw();

    /// Normalized right axis of the rendered view.
    public FVector3 Right => GetRightRaw();

    /// Normalized up axis of the rendered view.
    public FVector3 Up => GetUpRaw();

    /// Vertical field of view in degrees, 0 when nothing has been rendered yet.
    public float FieldOfView => GetFOVRaw();

    /// The entity holding the active SCameraComponent, or Entity.Null.
    public Entity Active => new(GetActiveEntityRaw());

    /// Makes an entity's camera the active view. A BlendTime above 0 runs the cinematic blend instead of cutting.
    public void SetActive(Entity Camera, float BlendTime = 0.0f,
        ECameraBlendFunction Blend = ECameraBlendFunction.EaseInOut)
        => SetActiveEntityRaw(Camera.Id, BlendTime, (int)Blend);

    /// Viewport size in pixels, zero on a world with no renderer.
    public FVector2 ViewportSize => GetViewportSizeRaw();

    /// Projects a world point to pixel coordinates, origin top-left. Check bOnScreen before you draw with it.
    public FScreenProjection WorldToScreen(FVector3 WorldLocation) => WorldToScreenRaw(WorldLocation);

    /// Pixel coordinates to a world ray, for picking and mouse-aimed shots.
    public FWorldRay ScreenToWorldRay(FVector2 ScreenPosition) => ScreenToWorldRayRaw(ScreenPosition);

    /// The ray through the middle of the viewport, which is where a crosshair sits.
    public FWorldRay CenterRay => ViewportCenterRayRaw();

    /// The ray under the mouse cursor.
    public FWorldRay MouseRay => ScreenToWorldRayRaw(new Input(Handle).MousePosition);

    /// A point WorldDistance along the ray through ScreenPosition, measured from the near plane.
    public FVector3 DeprojectScreenToWorld(FVector2 ScreenPosition, float WorldDistance)
        => DeprojectScreenToWorldRaw(ScreenPosition, WorldDistance);

    /// <summary>
    /// Start a camera shake and return a handle to stop it. <paramref name="LocationAmplitude"/> is the max
    /// local-space positional offset per axis (world units); <paramref name="RotationAmplitude"/> is the max
    /// rotation per axis in degrees (X = pitch, Y = yaw, Z = roll). <paramref name="Frequency"/> is the
    /// oscillation rate in Hz. A <paramref name="Duration"/> &lt;= 0 loops until the shake is stopped.
    /// </summary>
    public FCameraShake PlayShake(FVector3 LocationAmplitude, FVector3 RotationAmplitude,
        float Frequency = 10f, float Duration = 0.5f, float BlendIn = 0.05f, float BlendOut = 0.2f)
    {
        FCameraShakeWire Wire = new FCameraShakeWire
        {
            LocationAmplitude = LocationAmplitude,
            RotationAmplitude = RotationAmplitude,
            Frequency = Frequency,
            Duration = Duration,
            BlendInTime = BlendIn,
            BlendOutTime = BlendOut,
        };
        return new FCameraShake(Handle, PlayShakeRaw(Wire));
    }

    /// <summary>
    /// Quick impact/explosion shake: <paramref name="Intensity"/> scales a default rotational + positional
    /// kick (≈1 = a light bump, ≈5 = a heavy blast). Use the other overload for full control.
    /// </summary>
    public FCameraShake PlayShake(float Intensity, float Duration = 0.4f)
    {
        float BlendOut = Duration < 0.25f ? Duration : 0.25f;
        return PlayShake(
            new FVector3(Intensity * 0.04f, Intensity * 0.04f, Intensity * 0.02f),
            new FVector3(Intensity * 1.5f, Intensity * 1.5f, Intensity * 0.6f),
            18f, Duration, 0.03f, BlendOut);
    }

    /// <summary>Stop every active shake immediately.</summary>
    public void StopAllShakes() => StopAllShakesRaw();

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Camera_GetPosition")]
    private partial FVector3 GetPositionRaw();
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Camera_GetForward")]
    private partial FVector3 GetForwardRaw();
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Camera_GetRight")]
    private partial FVector3 GetRightRaw();
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Camera_GetUp")]
    private partial FVector3 GetUpRaw();
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Camera_GetFOV")]
    private partial float GetFOVRaw();
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Camera_GetActiveEntity")]
    private partial uint GetActiveEntityRaw();
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Camera_SetActiveEntity")]
    private partial void SetActiveEntityRaw(uint Entity, float BlendTime, int BlendFunction);

    // The projection shims are CWorld calls; this facade's Handle is that same CWorld pointer.
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Call_Lumina_CWorld_GetViewportSize")]
    private partial FVector2 GetViewportSizeRaw();
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Call_Lumina_CWorld_WorldToScreen")]
    private partial FScreenProjection WorldToScreenRaw(FVector3 WorldLocation);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Call_Lumina_CWorld_ScreenToWorldRay")]
    private partial FWorldRay ScreenToWorldRayRaw(FVector2 ScreenPosition);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Call_Lumina_CWorld_ViewportCenterRay")]
    private partial FWorldRay ViewportCenterRayRaw();
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Call_Lumina_CWorld_DeprojectScreenToWorld")]
    private partial FVector3 DeprojectScreenToWorldRaw(FVector2 ScreenPosition, float WorldDistance);

    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Camera_PlayShake")]
    private partial uint PlayShakeRaw(FCameraShakeWire Shake);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Camera_StopShake")]
    internal partial void StopShakeRaw(uint ShakeId);
    [NativeCall(Module = "Runtime", EntryPoint = "LuminaSharp_Camera_StopAllShakes")]
    private partial void StopAllShakesRaw();
}

/// <summary>
/// A live camera shake returned by <see cref="Camera.PlayShake"/>. Lightweight value handle; safe to copy.
/// Call <see cref="Stop"/> to end it (it fades out over its blend-out time).
/// </summary>
public readonly struct FCameraShake
{
    private readonly ulong World;

    /// <summary>Opaque handle; 0 means the shake wasn't created.</summary>
    public readonly uint Id;

    internal FCameraShake(ulong World, uint Id)
    {
        this.World = World;
        this.Id = Id;
    }

    public bool IsValid => Id != 0;

    /// <summary>Stop this shake; it fades out over its blend-out time rather than cutting abruptly.</summary>
    public void Stop()
    {
        if (Id != 0)
        {
            new Camera(World).StopShakeRaw(Id);
        }
    }
}

/// <summary>Blittable mirror of native FLmCameraShake (DotNetGameplay.cpp).</summary>
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("FCameraShakeWire")]
internal struct FCameraShakeWire
{
    public FVector3 LocationAmplitude;
    public FVector3 RotationAmplitude;
    public float Frequency;
    public float Duration;
    public float BlendInTime;
    public float BlendOutTime;
}
