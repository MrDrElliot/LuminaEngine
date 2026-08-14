"""Finds #include directives whose case does not match the file on disk.

    python BuildScripts/CheckIncludeCase.py           report
    python BuildScripts/CheckIncludeCase.py --fix     rewrite them

Windows resolves a wrong-case include anyway, so these accumulate invisibly and then fail all at
once the first time anyone builds on Linux -- 45 of them across 25 files, on the first attempt here.
Worth running before a Linux build, and worth running on Windows precisely because Windows is where
they are introduced and never noticed.

Note that a WSL build from /mnt is no substitute: that is a Windows filesystem and is equally
case-insensitive, so it accepts every one of them too.
"""
import os
import re
import sys

ROOT = r"H:/LuminaEngine/Engine"

# Include roots, in the order the compiler searches them. Only first-party roots plus the
# vendored trees that first-party code includes by path.
SEARCH_ROOTS = [
    "Source/Runtime/Source",
    "Editor/Source",
    "Applications/Reflector/Source",
    "Source/ThirdParty",
    "Source/ThirdParty/EA/EASTL/include",
    "Source/ThirdParty/EA/EABase/include/Common",
    "Source/ThirdParty/imgui",
    "Source/ThirdParty/entt",
    "Source/ThirdParty/json",
]

# Files whose includes we check (first-party only; vendored code is not ours to correct).
SCAN_DIRS = ["Source/Runtime", "Editor", "Applications", "Plugins"]
SKIP = ("ThirdParty", "imgui-node-editor", "/generated/", "\\Generated\\")

# Both quote forms. Angle-bracket includes are just as case-sensitive on Linux, and the engine
# uses them for the vendored trees (<EASTL/...>, <imgui.h>), which is exactly where the
# lowercase <eastl/...> spellings had accumulated.
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*(?:"([^"]+)"|<([^>]+)>)', re.MULTILINE)


def build_index():
    """actual-relative-path -> True, plus lowercase -> actual, per search root."""
    exact = set()
    lower = {}

    for root in SEARCH_ROOTS:
        base = os.path.join(ROOT, root)
        if not os.path.isdir(base):
            continue
        for dirpath, _, files in os.walk(base):
            for name in files:
                if not name.endswith((".h", ".hpp", ".inl")):
                    continue
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, base).replace("\\", "/")
                exact.add(rel)
                lower.setdefault(rel.lower(), rel)

    return exact, lower


def true_case(base, relative):
    """The on-disk spelling of every component of `relative`, or None if it does not resolve.

    Every component matters, not just the file name: "components/EntityTags.h" is still wrong when
    the directory is Components. Windows answers for the whole path at once, so a half-corrected
    include looks fixed there and still fails on Linux.
    """
    parts = relative.split("/")
    current = base
    resolved = []

    for part in parts:
        try:
            entries = os.listdir(current)
        except OSError:
            return None

        match = next((e for e in entries if e.lower() == part.lower()), None)
        if match is None:
            return None

        resolved.append(match)
        current = os.path.join(current, match)

    return "/".join(resolved)


def scan():
    exact, lower = build_index()
    findings = []

    for scan_dir in SCAN_DIRS:
        base = os.path.join(ROOT, scan_dir)
        if not os.path.isdir(base):
            continue
        for dirpath, _, files in os.walk(base):
            if any(s in dirpath.replace("\\", "/") for s in SKIP):
                continue
            for name in files:
                if not name.endswith((".h", ".hpp", ".cpp", ".inl")):
                    continue
                path = os.path.join(dirpath, name)
                try:
                    text = open(path, encoding="utf-8-sig").read()
                except (UnicodeDecodeError, OSError):
                    continue

                for quoted, angled in INCLUDE_RE.findall(text):
                    inc = quoted or angled
                    bAngled = not quoted
                    norm = inc.replace("\\", "/")

                    # A .generated.h is named after the including file, not something on disk yet.
                    if norm.endswith(".generated.h"):
                        want = os.path.splitext(name)[0] + ".generated.h"
                        if norm != want and norm.lower() == want.lower():
                            findings.append((path, inc, want))
                        continue

                    if norm in exact:
                        continue

                    # Relative to the including file's own directory? (quoted form only)
                    if not bAngled and os.path.isfile(os.path.join(dirpath, norm)):
                        real = true_case(dirpath, norm)
                        if real and real != norm:
                            findings.append((path, inc, real))
                        continue

                    match = lower.get(norm.lower())
                    if match and match != norm:
                        findings.append((path, inc, match))

    return findings


def main():
    findings = scan()
    fix = "--fix" in sys.argv

    by_file = {}
    for path, wrong, right in findings:
        by_file.setdefault(path, []).append((wrong, right))

    for path, items in sorted(by_file.items()):
        print(os.path.relpath(path, ROOT))
        for wrong, right in items:
            print(f"    {wrong}  ->  {right}")

        if fix:
            text = open(path, encoding="utf-8-sig").read()
            for wrong, right in items:
                text = text.replace(f'#include "{wrong}"', f'#include "{right}"')
                text = text.replace(f'#include <{wrong}>', f'#include <{right}>')
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)

    print(f"\n{len(findings)} case-mismatched include(s) in {len(by_file)} file(s)"
          + ("  [FIXED]" if fix else ""))


main()
