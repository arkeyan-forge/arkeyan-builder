# arkeyan-builder: the player kit's builder (SSABuild, then SkyBuild, before 0.8.0)

> **Purpose:** what this repository is, how it fits the forge, and its licence · **Status:** current · **Tags:** none

Arkeyan-Builder builds a native Skylanders port on the player's own machine from the player's own disc image: it recognises
which game the disc is, translates the game's code and shaders locally and compiles the result. It carries no game code
and no game file. Skylauncher runs it and reads its one-event-per-line output (`@step`, `@progress`, `@log`, `@error`,
`@done`); `README.txt.in` is the text shipped inside each kit.

This repository is a component of the **forge** (`arkeyan-forge/forge`), where it is mounted at `tools/ssabuild/`; build
it from there (`git clone --recursive https://github.com/arkeyan-forge/forge`), with the CMake targets and the player-kit
scripts (`tools/make_kit.sh`) described in the forge's documentation.

| File | What |
|---|---|
| `main.cpp`, `process.*` | the command line, the build steps and the process runner |
| `igz_shaders.*` | reading the shader containers of the Alchemy games' archives |
| `kit_commands.py` | the kit's helper commands |
| `sha256.*`, `xenos_mingw.h`, `ssabuild.rc` | hashing, the MinGW shader-translator shim and the Windows resource file |

Licence: MIT, see [LICENSE](LICENSE). Third-party notices are in the forge's `THIRD_PARTY_NOTICES.md`.
