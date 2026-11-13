#include "LuaBinding.h"

#include "Core/Module/ModuleManager.h"
#include "ReflectionExampleRuntime.lua.gen.h"

IMPLEMENT_MODULE(Lumina::IModuleInterface, "ReflectionExampleRuntime");

namespace Lumina::Lua
{
    TSpan<const FClassBinding> GetClassBindings()
    {
        return ReflectionExampleRuntime::Classes;
    }
}
