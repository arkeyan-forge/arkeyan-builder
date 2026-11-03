"""Pack Arkeyan-Builder's objects and link arguments, or relink it with replacement LGPL libraries.

make_kit.sh calls --pack. The kit includes this script and instructions in relink/README.txt.
No compiler or build-machine paths are saved in the link recipe.
"""
import argparse
import json
from pathlib import Path
import shlex
import shutil
import subprocess


def pack(build, kit):
    build, kit = build.resolve(), kit.resolve()
    output = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", "Arkeyan-Builder"], text=True)
    # CMake wraps the link in no-op shell segments on some platforms.
    parts = [shlex.split(part) for part in output.splitlines()[-1].split(" && ")]
    args = next(part for part in parts if "-o" in part)
    folder = kit / "relink" / "arkeyan-builder"
    folder.mkdir(parents=True, exist_ok=True)
    recipe = []
    i = 1  # the player's compiler is supplied at relink time
    while i < len(args):
        arg = args[i]
        if arg == "-o":
            recipe += ["-o", "${OUT}"]
            i += 1
        elif arg == "-isysroot":
            recipe += [arg, "${SDK}"]
            i += 1
        elif arg.startswith(("-ffile-prefix-map=", "-fmacro-prefix-map=", "-fdebug-prefix-map=", "-Wl,--out-implib")):
            pass
        elif not arg.startswith("-") and arg.endswith((".o", ".obj", ".res", ".a")):
            source = Path(arg)
            if not source.is_absolute():
                source = build / source
            # Separate names for the executable's objects and dependency libraries.
            name = arg.split("Arkeyan-Builder.dir/")[-1].replace("../", "").replace("/", "_") if arg.endswith((".o", ".obj", ".res")) else source.name
            shutil.copyfile(source, folder / name)
            recipe.append("${KIT}/relink/arkeyan-builder/" + name)
        elif ".sdk/usr/lib/lib" in arg and arg.endswith(".tbd"):
            recipe.append("-l" + Path(arg).name[3:-4])
        elif str(build) in arg or (Path(arg).is_absolute() and arg != "/usr/lib"):
            raise SystemExit("unmapped path in Arkeyan-Builder link recipe")
        else:
            recipe.append(arg)
        i += 1
    (folder.parent / "arkeyan-builder.json").write_text(json.dumps(recipe, indent=2) + "\n")
    shutil.copyfile(__file__, folder.parent / "arkeyan-builder.py")
    (folder.parent / "README.txt").write_text(
        "Arkeyan-Builder links the LGPL FFmpeg libraries statically. To use modified libraries,\n"
        "replace relink/arkeyan-builder/libavcodec.a and libavutil.a with compatible static builds\n"
        "for this kit's platform, then run from the kit folder:\n"
        "  python3 relink/arkeyan-builder.py --link . <C++ compiler> <output executable>\n"
        "On macOS add --sdk <SDK folder> if the recipe uses an SDK. Windows needs the\n"
        "matching llvm-mingw compiler; Linux needs the kit's compiler and target flags.\n"
        "Objects, dependencies and the original link recipe are included. The FFmpeg\n"
        "source, build script and LGPL text are in licenses/. Arkeyan-Builder's source is the\n"
        "project source identified by kit.txt's build field. No game data is included.\n")


def link(kit, compiler, output, sdk):
    kit = kit.resolve()
    recipe = json.loads((kit / "relink" / "arkeyan-builder.json").read_text())
    values = {"${KIT}": str(kit), "${OUT}": str(output.resolve()), "${SDK}": sdk or ""}
    if "${SDK}" in recipe and not sdk:
        raise SystemExit("this kit needs --sdk <SDK folder>")
    for key, value in values.items():
        recipe = [arg.replace(key, value) for arg in recipe]
    subprocess.run([compiler] + recipe, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--pack", nargs=2, metavar=("BUILD", "KIT"))
    modes.add_argument("--link", nargs=3, metavar=("KIT", "COMPILER", "OUTPUT"))
    parser.add_argument("--sdk")
    options = parser.parse_args()
    if options.pack:
        pack(*(Path(p) for p in options.pack))
    else:
        kit, compiler, output = options.link
        link(Path(kit), compiler, Path(output), options.sdk)
