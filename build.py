import os
import shutil
import platform
import subprocess
from pathlib import Path


REPO_DIR = Path(__file__).absolute().parent
BUILD_DIR = REPO_DIR / "build"
SRC_DIR = REPO_DIR / "src"


SYSTEM = platform.system()

if SYSTEM == "Darwin":
    libname = "libojph_capi.dylib"
elif SYSTEM == "Windows":
    libname = "ojph_capi.dll"
else:  # assume linux-like
    libname = "libojph_capi.so"


# Clean
shutil.rmtree(BUILD_DIR, ignore_errors=True)
shutil.rmtree(REPO_DIR / "dist", ignore_errors=True)
for p in SRC_DIR.iterdir():
    if p.name.endswith((".so", ".dll", ".dylib")):
        p.unlink()

subprocess.run(["cmake", "-S", REPO_DIR,  "-B", "build", "-DCMAKE_BUILD_TYPE=Release",
"-DCMAKE_INSTALL_PREFIX=dist"], check=True)


subprocess.run(["cmake","--build", "build"], check=True)


shutil.copy2(BUILD_DIR / libname, SRC_DIR / libname)


if SYSTEM == "Darwin":
    # Copy libopenjph.dylib into the same dir, and fix rpath
    subprocess.run(["delocate-path", "src", "-d", "-v", "-L", "."], check=True)
elif SYSTEM == "Linux":
    # TODO: fix
    run(["patchelf", "--set-rpath", "$ORIGIN", str(lib_path)])
elif SYSTEM == "Windows":
    # TODO: check
    pass  # delvewheel operates on wheels; nothing to do for raw .dll
else:
    print(f"Warning: unknown platform {SYSTEM}, skipping repair")


print("done!")
