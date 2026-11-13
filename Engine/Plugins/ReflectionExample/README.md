# ReflectionExample

A Reflector plugin that generates a whole binding layer from the reflection model, the way a Lua
plugin would generate Lua bindings. There is no Lua VM here; a small table-driven dispatch stands in
for one.

## Modules

- `ReflectionExampleGenerator` (Type `Reflection`) is loaded into the Reflector at generation time
  and never ships. `LuaBindingGenerator.cpp` claims the `LuaExport` specifier and, for every module
  with a `REFLECT(LuaExport)` class or struct, writes `<Module>.lua.gen.h` into that module's
  generated directory. Each class gets sorted getter, setter and method tables of `luaL_Reg`-style
  rows whose thunks touch the C++ members directly. Properties or functions with no Lua value are
  skipped with a `file(line)` warning.
- `ReflectionExampleRuntime` includes the generated header in `LuaBinding.cpp`, and `LuaBinding.h`
  holds the stack, value and lookup types the thunks use. `CLuaExampleCounter` is the exported class.

## Enabling

The plugin is off by default, so no shipped target carries it. Its own test suite enables it, since
a suite turns on the plugin of the module it tests. Turn it on for a build with
`-EnablePlugin=ReflectionExample`, or in a project's `.lproject`.
