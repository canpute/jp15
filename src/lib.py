import os
import sys
import atexit
import platform
from pathlib import Path
import importlib.resources
from contextlib import ExitStack

from cffi import FFI

LIB_ROOT = Path(__file__).absolute().parent
EXEC_PATH = Path(sys.exec_prefix)


# Our resources are most probably always on the file system. But in
# case they don't we have a nice exit handler to remove temporary files.
_resource_files = ExitStack()
atexit.register(_resource_files.close)



def get_resource(name, type:Literal["header", "lib", "other"]):

    def genpaths():
        # Try from resources
        try:
            ref = importlib.resources.files("jp15.resources") / name
        except ModuleNotFoundError:
            pass
        else:
            context = importlib.resources.as_file(ref)
            yield _resource_files.enter_context(context)

        # Try local dir, file gets there when compiled in dev mode
        yield LIB_ROOT / name

        # Conda or system based installations
        if type == "header":
            yield EXEC_PATH / "Library" / "include" / name
            yield EXEC_PATH / "include" / name
        elif type == "lib":
            yield EXEC_PATH / "Library" / "bin" / name
            yield EXEC_PATH / "lib" / name
            yield EXEC_PATH / "lib64" / name


    tried_paths = []
    for path in genpaths():
        if path.exists():
            return str(path)
        tried_paths.append(path)

    raise RuntimeError(f"Could not find the resource file {name!r}, tried {[str(p) for p in tried_paths]}")


def get_header():
    filename = get_resource("ojph_capi.h", "header")
    with open(filename, "rb") as f:
        source = f.read().decode()

    lines = []
    for line in source.splitlines():
        if line.startswith(("#ifdef", "#ifndef", "#endif", "#include", "#define OJPH_CAPI_H")):
            continue
        elif 'extern "C"' in line:
            continue
        lines.append(line)
    return "\n".join(lines)


def get_lib_path():
    """Get the path to the dynamic library, taking into account the
    JP15_LIB_PATH environment variable.
    """

    # If path is given, use that or fail trying
    override_path = os.getenv("JP15_LIB_PATH", "").strip()
    if override_path:
        return override_path


    # Get lib filename for supported platforms
    system = platform.system()
    if system == "Windows":  # no-cover
        lib_filename = "ojph_capi.dll"
    elif system == "Darwin":  # no-cover
        lib_filename = "libojph_capi.dylib"
    else:  # co-cover
        lib_filename = "libojph_capi.so"

    try:
        return get_resource(lib_filename, "lib")
    except RuntimeError as err:
        hints = [
            "You can set the JP15_LIB_PATH env var to the location of the library.",
            "If this is a dev-insall, maybe you just need to compile it first."
        ]
        msg = err.args[0] + "\n" + " ".join(hints)
        raise RuntimeError(msg)


ffi = FFI()
ffi.cdef(get_header())
ffi.set_source("ojph_capi.h", None)

path = get_lib_path()  # store path on this module so it can be checked
capi = ffi.dlopen(path)

openjph_version = ffi.string(capi.ojph_version()).decode()

