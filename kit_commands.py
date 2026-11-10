"""Turns a game-free runtime build tree into the player kit's lib/ and commands/ (tools/make_kit.sh).

    python3 -I kit_commands.py <game id> <runtime build dir> <stub ppc dir> <kit dir>

Run once per game of the kit (docs/release/launcher/kit_contract.md). The commands are the ones CMake/Ninja would run, with the
build machine's paths replaced by ${KIT}, ${CXX}, ${SDK}, ${PPC}, ${SRC}, ${OBJ}, ${OUT} and ${OBJECTS} (Arkeyan-Builder). They go to
commands/<game>/. The ArkeyanRuntime target's own objects are compiled against the game's ppc_config.h, so they are the game's:
lib/<game>/runtime/. The static libraries (SDL, plume, FFmpeg, ...) are the same for every game: lib/ holds one copy, shared
(a library that differs between games would go to lib/<game>/ and the game's link.args would name it there). The game code
library and the shader cache library are left out (Arkeyan-Builder compiles them from the player's disc and passes them as
${OBJECTS}).
"""
import filecmp
import os
import shlex
import shutil
import subprocess
import sys

game = sys.argv[1]
build, stub, kit = (os.path.abspath(p) for p in sys.argv[2:5])
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def commands(target):
    out = subprocess.run(["ninja", "-C", build, "-t", "commands", target], check=True, capture_output=True, text=True)
    return out.stdout.splitlines()


def compiler_part(line):
    # "a && b && :" -> the segment that runs the compiler
    for part in line.split(" && "):
        part = part.strip()
        if "clang" in part.split(" ")[0] or part.startswith(("/usr/bin/c++", "/opt/zig/zig ")):
            return shlex.split(part)
    raise SystemExit("no compiler command in: " + line)


def write_args(name, args):
    os.makedirs(os.path.join(kit, "commands", game), exist_ok=True)
    with open(os.path.join(kit, "commands", game, name), "w", newline="\n") as f:
        f.write("\n".join(args) + "\n")


def common(args):
    """Compiler and SDK placeholders, dependency-file flags dropped."""
    result = ["${CXX}"]
    i = 1
    while i < len(args):
        a = args[i]
        if a in ("-MD",):
            pass
        elif a.startswith(("-ffile-prefix-map=", "-fmacro-prefix-map=", "-fdebug-prefix-map=")):
            pass # the kit builder's paths; the player's compile has none to hide
        elif a == "-idirafter" and args[i + 1] == "/usr/include":
            i += 1 # the Linux kit image's headers for SDL's configure (tools/make_kit.sh); the game needs none
        elif a in ("-MT", "-MF"):
            i += 1
        elif a == "-isysroot":
            result += ["-isysroot", "${SDK}"]
            i += 1
        else:
            result.append(a)
        i += 1
    return result


def compile_args(line, includes):
    args = common(compiler_part(line))
    result = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-o":
            result += ["-o", "${OBJ}"]
            i += 1
        elif a == "-c":
            result += ["-c", "${SRC}"]
            i += 1
        elif a == "-include":
            result += ["-include", includes[args[i + 1]]]
            i += 1
        elif a.startswith("-I"):
            result.append("-I" + includes[a[2:]])
        else:
            result.append(a)
        i += 1
    return result


simde = os.path.join(root, "thirdparty/XenonRecomp/thirdparty/simde")
guard = os.path.join(root, "ArkeyanRecompLib/indirect_call_guard.h")
gpu = os.path.join(root, "runtime/gpu")

ppc_line = next(l for l in commands("ArkeyanRecompLib/libArkeyanRecompLib.a") if "ppc_func_mapping.cpp" in l and " -c " in l)
write_args("compile.args", compile_args(ppc_line, {
    stub: "${PPC}", simde: "${KIT}/include/simde", guard: "${KIT}/include/indirect_call_guard.h"}))
cache_line = next(l for l in commands("ArkeyanRuntime/gpu/libArkeyanShaderCache.a") if "shader_cache.cpp" in l and " -c " in l)
write_args("shader_cache.args", compile_args(cache_line, {gpu: "${KIT}/include"}))

link_line = commands("ArkeyanRuntime")[-1]
args = common(compiler_part(link_line))
os.makedirs(os.path.join(kit, "lib", game, "runtime"), exist_ok=True)
result = []
copied = set()
private = set()  # libraries that differ from another game's copy
i = 0
while i < len(args):
    a = args[i]
    if a == "-o":
        result += ["-o", "${OUT}"]
        i += 1
    elif a.startswith("-Wl,--out-implib"):
        pass
    elif a.endswith("libArkeyanRecompLib.a"):
        result.append("${OBJECTS}")
    elif a.endswith("libArkeyanShaderCache.a"):
        pass  # shader_cache.o is one of ${OBJECTS}
    elif not a.startswith("-") and a.endswith((".obj", ".o", ".res")):
        name = a.split("ArkeyanRuntime.dir/", 1)[-1].replace("/", "_")
        shutil.copyfile(os.path.join(build, a), os.path.join(kit, "lib", game, "runtime", name))
        result.append("${KIT}/lib/" + game + "/runtime/" + name)
    elif not a.startswith("-") and a.endswith(".a"):
        name = os.path.basename(a)
        source = a if os.path.isabs(a) else os.path.join(build, a)
        if name not in copied:
            shared = os.path.join(kit, "lib", name)
            if os.path.exists(shared) and not filecmp.cmp(source, shared, shallow=False):
                os.makedirs(os.path.join(kit, "lib", game), exist_ok=True)
                shutil.copyfile(source, os.path.join(kit, "lib", game, name))
                print("kit_commands: %s differs between games, kept per game" % name)
                copied.add(name)
                private.add(name)
            else:
                shutil.copyfile(source, shared)
                copied.add(name)
        result.append("${KIT}/lib/" + (game + "/" if name in private else "") + name)
    elif ".sdk/usr/lib/lib" in a and a.endswith(".tbd"):
        result.append("-l" + os.path.basename(a)[3:-4])  # macOS SDK library: the compiler driver finds it
    elif "/" in a and not a.startswith("-"):
        raise SystemExit("unexpected path in the link command: " + a)
    else:
        result.append(a)
    i += 1
write_args("link.args", result)
