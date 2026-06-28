-- premake5 setup: fetch + verify External.zip, configure git hooks, persist LUMINA_DIR.

local LuminaDir = os.getenv("LUMINA_DIR") or _MAIN_SCRIPT_DIR
include (path.join(LuminaDir, "BuildScripts/Logger"))


-- Dependency bundle, published as a GitHub release asset (kept out of the repo to avoid LFS).
local DEPENDENCY_URL =
    "https://github.com/MrDrElliot/LuminaEngine/releases/download/external-deps/External.zip"

local DEPENDENCY_FILENAME   = "External.zip"
local DEPENDENCY_MARKER_DIR = "External"
local GIT_HOOKS_PATH        = "BuildScripts/Hooks"

-- Checked before extract; empty skips the check. Refresh: Get-FileHash External.zip
local EXPECTED_SHA256 = "A3405839CC5A355AA6BEBCA74522C19C2306E6268D86C205B67402E721123276"

local DEPENDENCY_MANIFEST =
{
    { name = ".NET 10 runtime + hosting headers", use = "C# scripting host (LuminaSharp)" },
    { name = "LLVM/Clang 19 (libclang)",          use = "reflection codegen (Reflector)"  },
    { name = "Slang shader compiler",             use = "shader compilation -> SPIR-V"     },
    { name = "RenderDoc",                          use = "in-app GPU frame capture"         },
    { name = "Tracy",                              use = "CPU/GPU profiler"                 },
}


newoption
{
    trigger     = "force",
    description = "Force re-download even if External/ already exists",
}

newoption
{
    trigger     = "yes",
    description = "Skip the download confirmation prompt (non-interactive / CI)",
}

newoption
{
    trigger     = "no-open",
    description = "Do not open Lumina.slnx after setup finishes (consumed by Setup.bat)",
}


local function FormatBytes(bytes)
    if bytes >= 1024 * 1024 then
        return string.format("%.2f MB", bytes / (1024 * 1024))
    elseif bytes >= 1024 then
        return string.format("%.2f KB", bytes / 1024)
    end
    return tostring(bytes) .. " B"
end

local function DownloadArchive(url, dest)
    Logger.Info("Downloading from " .. url)
    Logger.Info("    -> " .. dest)

    local lastPercent = -1
    local result, err = http.download(url, dest, {
        progress = function(total, current)
            if total and total > 0 then
                local percent = math.floor((current / total) * 100)
                if percent ~= lastPercent then
                    lastPercent = percent
                    local bars = math.floor(percent / 2)
                    io.write(string.format(
                        "\r  [%s%s] %3d%%  (%s / %s)",
                        string.rep("#", bars),
                        string.rep("-", 50 - bars),
                        percent,
                        FormatBytes(current),
                        FormatBytes(total)))
                    io.flush()
                end
            end
        end,
    })
    io.write("\n")

    if result ~= "OK" then
        Logger.Error("Download failed: " .. tostring(result) .. " " .. tostring(err or ""))
        return false
    end

    Logger.Success("Download complete.")
    return true
end

local function ExtractArchive(archive, destDir)
    local ext = path.getextension(archive):lower()
    Logger.Info("Extracting " .. archive .. " -> " .. destDir)

    if ext == ".zip" then
        local ok, err = pcall(zip.extract, archive, destDir)
        if not ok then
            Logger.Error("Extraction failed: " .. tostring(err))
            return false
        end
        return true
    end

    if ext == ".7z" then
        local sevenZip = path.join(LuminaDir, "Tools/7zr.exe")
        if not os.isfile(sevenZip) then
            Logger.Error("Cannot extract .7z: " .. sevenZip .. " not found.")
            Logger.Info("Resolve by either:")
            Logger.Info("  (a) Drop 7zr.exe (https://www.7-zip.org/download.html) into Tools/")
            Logger.Info("  (b) Repackage the archive as .zip and update DEPENDENCY_URL")
            return false
        end
        local cmd = string.format('"%s" x -y "%s" -o"%s"',
            path.translate(sevenZip, "\\"),
            path.translate(archive, "\\"),
            path.translate(destDir, "\\"))
        local code = os.execute(cmd)
        if code == 0 or code == true then
            return true
        end
        Logger.Error("7zr.exe exited with status " .. tostring(code))
        return false
    end

    Logger.Error("Unsupported archive extension: " .. ext)
    return false
end

local function PersistEnvVar(name, value)
    if os.host() ~= "windows" then
        Logger.Info(string.format("Skipping %s persistence on non-Windows host.", name))
        return true
    end
    -- Persist for future shells; non-fatal if it fails.
    local cmd = string.format('setx %s "%s" >nul 2>&1', name, value)
    local ok = os.execute(cmd)
    if ok == 0 or ok == true then
        Logger.Success(string.format("%s -> %s (persisted via setx).", name, value))
        return true
    end
    Logger.Warning(string.format(
        "Could not persist %s via setx (this session only).", name))
    return false
end

-- Pull the 64-hex hash out of certutil/Get-FileHash output (tolerates spaced bytes).
local function ExtractSha256(text)
    if not text then return nil end
    for line in (text .. "\n"):gmatch("([^\r\n]*)\r?\n") do
        local hex = line:gsub("%s", ""):lower()
        if #hex == 64 and hex:match("^%x+$") then
            return hex
        end
    end
    return nil
end

local function ComputeSha256(filePath)
    if os.host() == "windows" then
        local winPath = path.translate(filePath, "\\")
        -- Get-FileHash first (clean hex), certutil fallback. Double apostrophes so a
        -- path like C:\Users\O'Brien\ doesn't close the quoted string early.
        local psPath = winPath:gsub("'", "''")
        local ps = os.outputof(string.format(
            'powershell -NoProfile -Command "(Get-FileHash \'%s\' -Algorithm SHA256).Hash"', psPath))
        local hex = ExtractSha256(ps)
        if hex then return hex end
        return ExtractSha256(os.outputof(string.format('certutil -hashfile "%s" SHA256', winPath)))
    end
    return ExtractSha256(os.outputof(string.format(
        'sha256sum "%s" 2>/dev/null || shasum -a 256 "%s" 2>/dev/null', filePath, filePath)))
end

local function VerifyChecksum(filePath, expected)
    local pinned = expected and expected ~= ""
    local actual = ComputeSha256(filePath)
    if not actual then
        -- A pinned hash that can't be computed must fail, not silently proceed.
        if pinned then
            Logger.Error("A SHA-256 is pinned, but the bundle's hash could not be computed.")
            Logger.Error("Refusing to extract an unverifiable bundle (no PowerShell/certutil?).")
            return false
        end
        Logger.Warning("Could not compute SHA-256 (no PowerShell/certutil?); integrity NOT verified.")
        return true
    end

    if not pinned then
        Logger.Warning("================================================================")
        Logger.Warning(" Dependency bundle integrity was NOT verified.")
        Logger.Warning(" No EXPECTED_SHA256 is recorded in BuildScripts/Actions/Setup.lua.")
        Logger.Warning(" Downloaded SHA-256:")
        Logger.Warning("   " .. actual)
        Logger.Warning(" Maintainer: paste this into EXPECTED_SHA256 to lock the bundle.")
        Logger.Warning("================================================================")
        return true
    end

    expected = expected:gsub("%s", ""):lower()
    if actual == expected then
        Logger.Success("SHA-256 verified: " .. actual)
        return true
    end

    Logger.Error("SHA-256 MISMATCH -- refusing to extract an untrusted bundle.")
    Logger.Error("  expected: " .. expected)
    Logger.Error("  actual:   " .. actual)
    return false
end

local function PrintManifest()
    Logger.Info("------------------------------------------------------------")
    Logger.Info(" External dependencies (prebuilt bundle, ~671 MB)")
    Logger.Info("------------------------------------------------------------")
    for _, dep in ipairs(DEPENDENCY_MANIFEST) do
        Logger.Info(string.format("   - %-34s %s", dep.name, dep.use))
    end
    Logger.Info("")
    Logger.Info(" Source : " .. DEPENDENCY_URL)
    Logger.Info(" Details: DEPENDENCIES.md (upstream sources, versions, licenses)")
    Logger.Info("")
end

-- Only prompts on a direct `premake5 setup`; Setup.bat asks first and passes --yes.
local function ConfirmDownloadOrExit()
    if _OPTIONS["yes"] or os.getenv("LUMINA_SETUP_YES") then
        return
    end
    PrintManifest()
    io.write("Proceed with download? [Y/n] ")
    io.flush()
    local answer = io.read()
    if answer == nil then
        Logger.Warning("No input on stdin; pass --yes or set LUMINA_SETUP_YES=1 to confirm non-interactively.")
        os.exit(1)
    end
    answer = answer:gsub("%s", ""):lower()
    if answer == "n" or answer == "no" then
        Logger.Warning("Setup cancelled by user. No files were downloaded.")
        os.exit(1)
    end
end

local function ConfigureGitHooks()
    if not os.isdir(path.join(LuminaDir, ".git")) then
        Logger.Warning("Not a git repository; skipping hooks configuration.")
        return true
    end
    if not os.isdir(path.join(LuminaDir, GIT_HOOKS_PATH)) then
        Logger.Warning(GIT_HOOKS_PATH .. " not found; skipping hooks configuration.")
        return true
    end
    local cmd = string.format('git config core.hooksPath "%s"', GIT_HOOKS_PATH)
    local code = os.execute(cmd)
    if code == 0 or code == true then
        Logger.Success("Git hooks path -> " .. GIT_HOOKS_PATH)
        return true
    end
    Logger.Warning("Failed to configure git hooks path (non-fatal).")
    return false
end


newaction {
    trigger     = "setup",
    description = "First-time setup: download External dependencies, configure environment and hooks.",

    execute = function()
        Logger.Info("==========================================================")
        Logger.Info("                 LUMINA ENGINE SETUP                      ")
        Logger.Info("==========================================================")
        Logger.Info("Working directory: " .. LuminaDir)

        Logger.Info("")
        Logger.Info("[1/4] Configuring environment")
        PersistEnvVar("LUMINA_DIR", LuminaDir)

        Logger.Info("")
        Logger.Info("[2/4] External dependencies")
        local externalDir = path.join(LuminaDir, DEPENDENCY_MARKER_DIR)
        local archivePath = path.join(LuminaDir, DEPENDENCY_FILENAME)
        local skip        = os.isdir(externalDir) and not _OPTIONS["force"]

        if skip then
            Logger.Success("External/ already present; skipping download. Use --force to refresh.")
        else
            ConfirmDownloadOrExit()
            if not DownloadArchive(DEPENDENCY_URL, archivePath) then
                os.exit(1)
            end
            if not VerifyChecksum(archivePath, EXPECTED_SHA256) then
                os.remove(archivePath)
                Logger.Error("Deleted the failed download. Setup aborted.")
                os.exit(1)
            end
            if not ExtractArchive(archivePath, LuminaDir) then
                Logger.Error("Setup aborted; leaving " .. DEPENDENCY_FILENAME .. " in place for inspection.")
                os.exit(1)
            end
            os.remove(archivePath)
            Logger.Success("Dependencies installed.")
        end

        Logger.Info("")
        Logger.Info("[3/4] Git hooks")
        ConfigureGitHooks()

        Logger.Info("")
        Logger.Info("[4/4] Done")
        Logger.Success("Setup complete.")
        Logger.Info("Next: project files will be generated automatically.")
    end,
}
