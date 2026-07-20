using System.Runtime.InteropServices;
using Lumina;
using LuminaSharp.Rendering;

namespace LuminaSharp;

/// <summary>
/// Blittable mirror of the native DotNet::FManagedSceneView (ManagedRenderScene.h): the camera snapshot
/// handed to <see cref="RenderScene.OnExtract"/> each frame. Matrices are column-major, left-handed,
/// +Z forward, zero-to-one clip depth (the engine's conventions).
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct SceneView
{
    public FMatrix View;
    public FMatrix Projection;
    public FMatrix ViewProjection;
    public FVector3 Position;   public float FOV;
    public FVector3 Forward;    public float Near;
    public FVector3 Up;         public float Far;
    public FVector3 Right;      public float AspectRatio;
}

/// <summary>
/// A world renderer authored in C#. Declare exactly one non-abstract subclass in your scripts and the
/// engine renders every Game world through it instead of the built-in renderer (editor and utility worlds
/// keep the engine renderer). Record and submit GPU work with the <c>RHI</c> API; the engine blits
/// <see cref="DisplayTexture"/> to the swapchain (game) or samples <see cref="DisplayResourceID"/>
/// (editor viewport).
///
/// Threading: <see cref="OnExtract"/> runs on the game thread; <see cref="OnRender"/>,
/// <see cref="DisplayTexture"/> and <see cref="DisplayResourceID"/> run on the render thread and must not
/// await or block. OnRender's FrameIndex cycles over the engine's frames-in-flight; use it to ring
/// per-frame resources when overlapping frames.
///
/// Hot reload: the engine shuts this instance down (OnShutdown) before scripts reload and creates a fresh
/// instance of the new generation's type afterward. Free every GPU resource you created in OnShutdown.
/// </summary>
public abstract class RenderScene
{
    /// The world this scene renders.
    public Lumina.CWorld World { get; internal set; } = null!;

    /// Create GPU resources. Called once on the game thread, after the world is available.
    public virtual void OnInit()
    {
    }

    /// Free every GPU resource created by this instance. Called on the game thread with rendering flushed.
    public virtual void OnShutdown()
    {
    }

    /// Game-thread frame snapshot: capture the camera + whatever world state rendering needs.
    public virtual void OnExtract(in SceneView View)
    {
    }

    /// Render-thread: record and submit this frame's GPU work.
    public abstract void OnRender(int FrameIndex);

    /// The render target was resized (viewport or window change); recreate size-dependent resources.
    public virtual void OnResize(uint Width, uint Height)
    {
    }

    /// The final image the engine presents/composites. Invalid = nothing to show.
    public virtual FTextureH DisplayTexture => FTextureH.Invalid;

    /// Global-heap slot of the final image, for the editor viewport. uint.MaxValue = none.
    public virtual uint DisplayResourceID => uint.MaxValue;

    /// Pixel size of the render target. Never return zero.
    public abstract FUIntVector2 RenderExtent { get; }
}
