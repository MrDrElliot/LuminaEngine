using System;
using Lumina;

namespace LuminaSharp;

/** The engine's process-wide events, bound the same way a component event is. */
public static class CoreEvents
{
    /** Fires after a world is torn down, before the next one exists. */
    public static ScriptDelegate PostWorldUnload => Events.PostWorldUnload;

    /** Fires once as the engine begins shutting down, after which listeners are dropped. */
    public static ScriptDelegate PreEngineShutdown => Events.OnPreEngineShutdown;

    /** Fires when gameplay asks to quit; under the editor this ends PIE rather than the process. */
    public static ScriptDelegate GameQuitRequested => Events.OnGameQuitRequested;

    /** Fires each frame after the OS event pump, before input actions are evaluated. */
    public static ScriptDelegate InputPumped => Events.OnInputPumped;

    private static SCoreDelegates Events => new SCoreDelegates((nint)CCoreDelegateLibrary.GetCoreDelegates());
}
