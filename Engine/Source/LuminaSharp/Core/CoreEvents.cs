using System;
using Lumina;

namespace LuminaSharp;

/** The engine's process-wide events, bound the same way a component event is. */
public static class CoreEvents
{
    /** Fires after a world is torn down, before the next one exists. */
    public static ScriptDelegate PostWorldUnload => View(CCoreDelegateLibrary.GetPostWorldUnloadEvent());

    /** Fires once as the engine begins shutting down, after which listeners are dropped. */
    public static ScriptDelegate PreEngineShutdown => View(CCoreDelegateLibrary.GetPreEngineShutdownEvent());

    /** Fires when gameplay asks to quit; under the editor this ends PIE rather than the process. */
    public static ScriptDelegate GameQuitRequested => View(CCoreDelegateLibrary.GetGameQuitRequestedEvent());

    /** Fires each frame after the OS event pump, before input actions are evaluated. */
    public static ScriptDelegate InputPumped => View(CCoreDelegateLibrary.GetInputPumpedEvent());

    private static unsafe ScriptDelegate View(long Address) => new ScriptDelegate((void*)(nint)Address);
}
