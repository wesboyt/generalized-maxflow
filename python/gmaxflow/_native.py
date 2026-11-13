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

_INT_P = ctypes.POINTER(ctypes.c_int)
_DOUBLE_P = ctypes.POINTER(ctypes.c_double)
_HAS_ARRAYS = hasattr(_lib, "gmf_solve_arrays_flows")
if _HAS_ARRAYS:
    _lib.gmf_solve_arrays_flows.restype = ctypes.c_int
    _lib.gmf_solve_arrays_flows.argtypes = [
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        _INT_P, _INT_P, _DOUBLE_P, _DOUBLE_P, _DOUBLE_P, _DOUBLE_P,
    ]


def _as_c(values, ctype, count):
    """A ctypes view of `values` without copying when it already is contiguous
    memory of the right type (a numpy array, array.array, memoryview); else a
    copy. Returns (pointer, keep-alive object)."""
    try:
        view = memoryview(values)
    except TypeError:
        view = None
    if (
        view is not None
        and view.contiguous
        and view.ndim == 1
        and view.itemsize == ctypes.sizeof(ctype)
        and view.format in (("i", "l") if ctype is ctypes.c_int else ("d",))
        and len(view) == count
        and not view.readonly
    ):
        buffer = (ctype * count).from_buffer(view)
        return ctypes.cast(buffer, ctypes.POINTER(ctype)), (view, buffer)
    copy = (ctype * count)(*values)
    return ctypes.cast(copy, ctypes.POINTER(ctype)), copy


def solve_arrays(num_nodes, source, sink, frm, to, capacity, multiplier, edge_flow):
    """Returns (status code, max flow). Writes per-edge flows into `edge_flow`
    (a writable float64 buffer of len(frm)) when it is not None."""
    if not _HAS_ARRAYS:
        raise RuntimeError("the loaded native library predates gmf_solve_arrays_flows")
    count = len(frm)
    if not (len(to) == len(capacity) == len(multiplier) == count):
        raise ValueError("from, to, capacity and multiplier must have the same length")
    keep = []
    pointers = []
    for values, ctype in ((frm, ctypes.c_int), (to, ctypes.c_int),
                          (capacity, ctypes.c_double), (multiplier, ctypes.c_double)):
        pointer, alive = _as_c(values, ctype, count)
        pointers.append(pointer)
        keep.append(alive)
    flows = None
    if edge_flow is not None:
        flows, alive = _as_c(edge_flow, ctypes.c_double, count)
        if not isinstance(alive, tuple):
            raise ValueError("edge_flow must be a writable float64 buffer of len(from) entries")
        keep.append(alive)
    out = ctypes.c_double(0.0)
    status = _lib.gmf_solve_arrays_flows(
        int(num_nodes), int(source), int(sink), count, *pointers, ctypes.byref(out), flows
    )
    return status, out.value


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
