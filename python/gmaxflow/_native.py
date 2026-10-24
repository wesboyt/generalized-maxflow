"""ctypes binding to the native solver. Internal; use the public functions.

The solver is a shared library with a plain C ABI, so it never links against
libpython: one wheel per OS and CPU serves every CPython, free-threaded builds
included, and PyPy. ctypes releases the GIL for the length of each call, so
other threads keep running while a large graph is solved.
"""

from __future__ import annotations

import ctypes
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ENV = "GMAXFLOW_LIB"


def _library_path() -> str:
    """$GMAXFLOW_LIB if set, else the library inside the installed package."""
    override = os.environ.get(_ENV)
    if override:
        return override
    name = {"win32": "gmaxflow_core.dll", "darwin": "gmaxflow_core.dylib"}.get(
        sys.platform, "gmaxflow_core.so"
    )
    return os.path.join(_HERE, "_native", name)


def _load() -> ctypes.CDLL:
    path = _library_path()
    if not os.path.exists(path):
        raise ImportError(
            "the gmaxflow native solver was not found (%s). Install a wheel "
            "(`pip install generalized-maxflow`), or build from a checkout with "
            "`pip install .`; a library built by hand with CMake can be named in "
            "$%s." % (path, _ENV)
        )
    return ctypes.CDLL(path)


_lib = _load()

_lib.gmf_solve_json_n.restype = ctypes.c_void_p
_lib.gmf_solve_json_n.argtypes = [
    ctypes.c_char_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.gmf_free.restype = None
_lib.gmf_free.argtypes = [ctypes.c_void_p]
_lib.gmf_version.restype = ctypes.c_char_p
_lib.gmf_version.argtypes = []


def solve_json(request: str) -> str:
    """Solve the JSON request and return the JSON answer."""
    data = request.encode("utf-8")
    size = ctypes.c_size_t(0)
    answer = _lib.gmf_solve_json_n(data, len(data), ctypes.byref(size))
    if not answer:
        raise MemoryError("the solver ran out of memory")
    try:
        return ctypes.string_at(answer, size.value).decode("utf-8")
    finally:
        _lib.gmf_free(answer)


def version() -> str:
    return _lib.gmf_version().decode("ascii")


def library_path() -> str:
    """Where the loaded native library lives, for diagnostics."""
    return _library_path()
