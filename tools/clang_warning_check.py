#!/usr/bin/env python3
"""Parse the project sources as the clang Windows build would, on a GCC tree.

    tools/clang_warning_check.py [build-dir] [--jobs N] [--all]

WHY THIS EXISTS

The native build here is GCC; the Windows, macOS and Android builds are
clang, all with -Werror. Clang warns about things GCC has no flag for -
-Wunused-lambda-capture above all, a [this] left on a handler after its
body stopped using it - and <windows.h> defines near and far as empty
macros, so a local named far is a syntax error there. A tree that builds
and passes every test here still stopped the Windows build on twelve
unused captures, three unused constants, a member-array size used as a
constant and two locals named far, found only when the Windows package
was rebuilt.

HOW

Take the real compile command of every project translation unit (the
-Werror one where a source is compiled twice), swap the compiler for
clang++, add the clang-only warning the CMake build turns on for clang
(-Wunused-const-variable), and for the client target -Dnear= -Dfar=;
drop the GCC PCH and LTO flags, and ask for -fsyntax-only.

It cannot see the rest of <windows.h> (min/max are off by NOMINMAX, but
ERROR, PlaySound and friends are not emulated) nor code under _WIN32.

A full pass is minutes, so a translation unit that parsed clean is
remembered in <build-dir>/clang_warning_check.cache under a hash of its
command, its source and every project header ninja recorded as a
dependency. The next run parses only what changed; --all ignores the
cache. Without ninja's dependency log the sources alone are hashed.

clang++ comes from $WOWEE_CLANGXX (the nix devShell sets it without
putting clang on PATH, so CMake still picks GCC), else clang++ on PATH.
With neither, the test is skipped (exit 77).
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKIP = 77

# GCC-only, or meaningless for a parse: dropped with no argument.
DROP = ("-Winvalid-pch", "-fno-fat-lto-objects", "-Wno-odr", "-MD", "-MMD")
DROP_PREFIX = ("-flto", "-fconcepts-diagnostics-depth")
# Dropped together with the argument after them.
DROP_WITH_ARG = ("-o", "-MT", "-MF", "-MQ")
# What the CMake build adds only when the compiler is clang.
CLANG_ONLY = ["-Wunused-const-variable"]
# <windows.h> (minwindef.h) defines near and far as empty macros, so a local
# named far parses on Linux and is a syntax error in the Windows build. Only
# for the client target, where SDL and Vulkan bring <windows.h> in; most tests
# never include it and name their locals near and far freely.
WINDOWS_MACROS = ["-Dnear=", "-Dfar="]
WINDOWS_MACRO_TARGET = "/CMakeFiles/wowee.dir/"
# The GCC .gch next to cmake_pch.hxx is ignored and the header parsed instead;
# a nix wrapper's linker flags are unused by a parse. Neither is the code's.
QUIET = ["-Wno-ignored-gch", "-Wno-unused-command-line-argument"]


def find_clang():
    for cand in (os.environ.get("WOWEE_CLANGXX"), shutil.which("clang++")):
        if cand and shutil.which(cand):
            return cand
    return None


def clang_command(entry, clang):
    args = shlex.split(entry.get("command") or " ".join(entry["arguments"]))
    out, i = [clang], 1
    while i < len(args):
        arg = args[i]
        if arg in DROP_WITH_ARG:
            i += 2
            continue
        if arg in DROP or arg.startswith(DROP_PREFIX):
            i += 1
            continue
        if arg == "-include" and i + 1 < len(args) and \
                args[i + 1].endswith("cmake_pch.hxx"):
            # GCC's .gch is not a clang PCH; clang falls back to the header,
            # which the sources may lean on for their includes.
            out += ["-include", args[i + 1]]
            i += 2
            continue
        if arg == "-c":
            out.append("-fsyntax-only")
            i += 1
            continue
        out.append(arg)
        i += 1
    if WINDOWS_MACRO_TARGET in entry.get("output", "") + " ".join(args):
        out += WINDOWS_MACROS
    return out + CLANG_ONLY + QUIET


def ninja_deps(build_dir):
    """object -> [dependency paths], from ninja's log; {} if unavailable."""
    ninja = shutil.which("ninja")
    if not ninja or not (build_dir / ".ninja_deps").is_file():
        return {}
    res = subprocess.run([ninja, "-C", str(build_dir), "-t", "deps"],
                         capture_output=True, text=True, errors="replace")
    deps, cur = {}, None
    for line in res.stdout.splitlines():
        if not line.strip():
            cur = None
        elif not line.startswith(" "):
            cur = line.split(":", 1)[0]
            deps[cur] = []
        elif cur is not None:
            deps[cur].append(line.strip())
    return deps


def fingerprint(command, source, deps):
    h = hashlib.sha256(" ".join(command).encode())
    root = str(ROOT)
    files = [source] + sorted(d for d in deps if d.startswith(root))
    for f in files:
        try:
            h.update(f.encode() + b"\0" + pathlib.Path(f).read_bytes())
        except OSError:
            h.update(f.encode() + b"\0missing")
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build_dir", nargs="?", default=str(ROOT / "build"))
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--all", action="store_true", help="ignore the cache")
    opts = ap.parse_args()

    clang = find_clang()
    if not clang:
        print("clang_warning_check: no clang++ ($WOWEE_CLANGXX or PATH); skipped")
        return SKIP
    build_dir = pathlib.Path(opts.build_dir).resolve()
    db = build_dir / "compile_commands.json"
    if not db.is_file():
        print("clang_warning_check: no " + str(db) + "; configure first")
        return 1

    # Every project translation unit once, by its -Werror command where it
    # has one (tests compile some sources a second time without it).
    compiler_id = ""
    by_file = {}
    for entry in json.load(open(db)):
        command = entry.get("command") or " ".join(entry["arguments"])
        name = entry["file"]
        if "-x c++-header" in command or not name.endswith((".cpp", ".cc", ".cxx")):
            continue
        if not name.startswith(str(ROOT) + "/") or \
                name.startswith((str(ROOT / "extern") + "/", str(build_dir) + "/")):
            continue
        compiler_id = compiler_id or command.split()[0]
        if name not in by_file or ("-Werror" in command and
                                   "-Werror" not in by_file[name]["command"]):
            entry["command"] = command
            by_file[name] = entry
    entries = list(by_file.values())
    if "clang" in pathlib.Path(compiler_id).name:
        print("clang_warning_check: this tree is built with clang already")
        return 0

    deps = ninja_deps(build_dir)
    cache_path = build_dir / "clang_warning_check.cache"
    try:
        cache = {} if opts.all else json.loads(cache_path.read_text())
    except (OSError, ValueError):
        cache = {}

    jobs = []
    for entry in entries:
        command = clang_command(entry, clang)
        output = entry.get("output", "")
        if not output:
            args = shlex.split(entry.get("command") or "")
            output = args[args.index("-o") + 1] if "-o" in args else entry["file"]
        if os.path.isabs(output):
            output = os.path.relpath(output, entry["directory"])
        key = fingerprint(command, entry["file"], deps.get(output, []))
        if cache.get(entry["file"]) == key:
            continue
        jobs.append((entry, command, key))

    print("clang_warning_check: %d of %d translation units to parse with %s"
          % (len(jobs), len(entries), clang))

    def run(job):
        entry, command, key = job
        res = subprocess.run(command, cwd=entry["directory"],
                             capture_output=True, encoding="utf-8",
                             errors="replace",
                             preexec_fn=lambda: os.nice(19))
        return entry, key, res

    failed = []
    with concurrent.futures.ThreadPoolExecutor(max(1, opts.jobs)) as pool:
        for entry, key, res in pool.map(run, jobs):
            if res.returncode == 0:
                cache[entry["file"]] = key
            else:
                cache.pop(entry["file"], None)
                failed.append((entry["file"], res.stderr.strip()))

    try:
        cache_path.write_text(json.dumps(cache, indent=0, sort_keys=True))
    except OSError:
        pass

    if failed:
        for name, stderr in failed:
            print("\n--- " + os.path.relpath(name, ROOT) + " ---\n" + stderr)
        print("\n%d translation unit(s) the clang builds (Windows, macOS,"
              " Android) would reject." % len(failed))
        return 1
    print("clang_warning_check: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
