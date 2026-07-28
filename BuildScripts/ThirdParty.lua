-- Single source of truth for third-party libs (includes/defines/links per name); pure data, creates no projects. A module only sees a lib in its transitive dependency closure.

assert(LuminaConfig, "ThirdParty.lua must be included after BuildScripts/Dependencies")

LuminaThirdParty = LuminaThirdParty or {}
LuminaThirdParty.Registry = LuminaThirdParty.Registry or {}


-- Absolute if it has a drive (":") or premake token ("%{...}"); else relative to ThirdParty dir.
local function ResolveDir(Dir)
    if Dir:find("^%%{") or Dir:find(":") then
        return Dir
    end
    return LuminaConfig.ThirdPartyPath(Dir)
end


function LuminaThirdParty.Register(Def)
    assert(Def.Name, "LuminaThirdParty.Register: Name is required")

    Def.ResolvedIncludeDirs = {}
    for _, Dir in ipairs(Def.IncludeDirs or {}) do
        table.insert(Def.ResolvedIncludeDirs, ResolveDir(Dir))
    end

    Def.Defines      = Def.Defines or {}
    Def.Dependencies = Def.Dependencies or {}
    if Def.Link == nil then
        Def.Link = true
    end

    LuminaThirdParty.Registry[Def.Name] = Def
end


local function AppendUnique(Out, Seen, Value)
    if not Seen[Value] then
        Seen[Value] = true
        table.insert(Out, Value)
    end
end


-- Resolve dependency names into transitive deduplicated (includes, defines, links); unknown names pass through as raw links.
function LuminaThirdParty.Resolve(Names)
    local Includes, Defines, Links = {}, {}, {}
    local SeenInc, SeenDef, SeenLink, Visited = {}, {}, {}, {}

    local function Visit(Name)
        if Visited[Name] then return end
        Visited[Name] = true

        local Entry = LuminaThirdParty.Registry[Name]
        if not Entry then
            AppendUnique(Links, SeenLink, Name)
            return
        end

        for _, Dir in ipairs(Entry.ResolvedIncludeDirs) do AppendUnique(Includes, SeenInc, Dir) end
        for _, Def in ipairs(Entry.Defines)            do AppendUnique(Defines, SeenDef, Def) end

        if Entry.Link == true then
            AppendUnique(Links, SeenLink, Entry.Name)
        elseif type(Entry.Link) == "string" then
            AppendUnique(Links, SeenLink, Entry.Link)
        elseif type(Entry.Link) == "table" then
            for _, Lib in ipairs(Entry.Link) do AppendUnique(Links, SeenLink, Lib) end
        end

        for _, Dep in ipairs(Entry.Dependencies) do Visit(Dep) end
    end

    for _, Name in ipairs(Names or {}) do Visit(Name) end
    return Includes, Defines, Links
end


-- Convenience: just the include dirs for a dependency list (transitive).
function LuminaThirdParty.IncludesOf(Names)
    local Includes = LuminaThirdParty.Resolve(Names)
    return Includes
end


-- Foundation: headers the engine's public API exposes pervasively; linkable ones are linked by whichever module references their symbols.
LuminaThirdParty.Register({ Name = "EA",            IncludeDirs = { "EA/EASTL/include", "EA/EABase/include/Common" } })
LuminaThirdParty.Register({ Name = "Entt",          IncludeDirs = { "entt" },               Link = false })
-- SPDLOG_COMPILED_LIB must be public: without it spdlog's headers switch to header-only
-- and every dependent TU re-parses fmt's format-inl.h (~0.8s each).
LuminaThirdParty.Register({ Name = "SPDLog",        IncludeDirs = { "spdlog/include" },     Defines = { "SPDLOG_COMPILED_LIB" } })
LuminaThirdParty.Register({ Name = "NlohmannJson",  IncludeDirs = { "json" },               Link = false })
LuminaThirdParty.Register({ Name = "StbImage",      IncludeDirs = { "stb_image" },          Link = false })
LuminaThirdParty.Register({ Name = "RenderDoc",     IncludeDirs = { "RenderDoc" },          Link = false })
-- "." needed: included as <concurrentqueue/concurrentqueue.h>.
LuminaThirdParty.Register({ Name = "ConcurrentQueue",IncludeDirs = { ".", "concurrentqueue" }, Link = false })
LuminaThirdParty.Register({ Name = "RPMalloc",      IncludeDirs = { "rpmalloc" } })
LuminaThirdParty.Register({ Name = "XXHash",        IncludeDirs = { "xxhash" } })
LuminaThirdParty.Register({ Name = "Miniz",         IncludeDirs = { "miniz" } })
LuminaThirdParty.Register({ Name = "Tracy",         IncludeDirs = { "tracy/public" } })

-- Windowing / UI. "." entries put the ThirdParty root on the path for dir-prefixed includes (<imgui/misc/...>).
LuminaThirdParty.Register({ Name = "GLFW",          IncludeDirs = { "glfw/include" } })
LuminaThirdParty.Register({ Name = "ImGui",         IncludeDirs = { "imgui", "." },         Dependencies = { "GLFW" } })
LuminaThirdParty.Register({ Name = "FreeType",      IncludeDirs = { "FreeType/include" } })
LuminaThirdParty.Register({ Name = "RmlUi",         IncludeDirs = { "RmlUi/Include" },      Dependencies = { "FreeType" } })

-- Rendering
LuminaThirdParty.Register({ Name = "Vulkan",        IncludeDirs = { "vulkan" },             Link = false })
LuminaThirdParty.Register({ Name = "Volk",          IncludeDirs = { "." } })  -- <volk/volk.h>
LuminaThirdParty.Register({ Name = "VMA",           IncludeDirs = { "VulkanMemoryAllocator" }, Link = false })
-- Headers only; SLang import libs are wired via the Runtime module's ExtraLinks/LibDirs.
LuminaThirdParty.Register({ Name = "SLang",         IncludeDirs = { "SLang" },              Link = false })

-- Audio / physics / navigation
LuminaThirdParty.Register({ Name = "MiniAudio",     IncludeDirs = { "." } })  -- <MiniAudio/miniaudio.h>
LuminaThirdParty.Register({ Name = "JoltPhysics",   IncludeDirs = { "JoltPhysics" } })
LuminaThirdParty.Register({ Name = "Recast",        IncludeDirs = { "Recast/Recast/Include", "Recast/Detour/Include" } })

-- Networking. ENet needs the Winsock + multimedia-timer system libs.
LuminaThirdParty.Register({ Name = "ENet",          IncludeDirs = { "enet/include" }, Link = { "ENet", "ws2_32", "winmm" } })

-- Scripting. Header-only: FDotNetHost LoadLibrary's hostfxr at runtime, so there is no import lib.
LuminaThirdParty.Register({ Name = "DotNetHost",    IncludeDirs = { LuminaConfig.EnginePath("External/DotNet/include") }, Link = false })


-- MSDF font-atlas baking. "." needed: included as <msdfgen/...>.
LuminaThirdParty.Register({ Name = "MSDFGen",       IncludeDirs = { "." },                  Dependencies = { "FreeType" } })

-- Geometry / mesh / texture processing
LuminaThirdParty.Register({ Name = "MeshOptimizer", IncludeDirs = { "meshoptimizer/src" } })
LuminaThirdParty.Register({ Name = "MikkTSpace",    IncludeDirs = { "MikkTSpace/src" } })
LuminaThirdParty.Register({ Name = "BasicUniversal",IncludeDirs = { "basis_universal" } })

-- Model-format importers (editor-only). "." needed: dir-prefixed includes (<tinyobjloader/...>, "OpenFBX/ofbx.h").
LuminaThirdParty.Register({ Name = "TinyOBJLoader", IncludeDirs = { "." } })
LuminaThirdParty.Register({ Name = "OpenFBX",       IncludeDirs = { "." } })
LuminaThirdParty.Register({ Name = "FastGLTF",      IncludeDirs = { "fastgltf/include" } })


-- Dependency groups: what each module's public headers expose, so dependents compile against the same third-party headers.

-- Everything Runtime links or exposes through its public headers.
LuminaThirdParty.RuntimePublicDeps =
{
    "EA", "Entt", "SPDLog", "NlohmannJson", "StbImage", "RenderDoc",
    "ConcurrentQueue", "RPMalloc", "XXHash", "Miniz", "Tracy",
    "GLFW", "ImGui", "FreeType", "RmlUi",
    "Vulkan", "Volk", "VMA", "SLang",
    "MiniAudio", "JoltPhysics", "Recast",
    "ENet",
    "DotNetHost",
    "MeshOptimizer", "MikkTSpace", "BasicUniversal", "MSDFGen",
}

-- Extra third-party the Editor module exposes on top of Runtime's set.
LuminaThirdParty.EditorPublicDeps =
{
    "TinyOBJLoader", "OpenFBX", "FastGLTF",
}
