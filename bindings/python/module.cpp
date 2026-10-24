// CPython extension module: the JSON interface, nothing more.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <exception>
#include <string>

#include "gmf/gmf.hpp"

namespace {

PyObject* solve_json(PyObject* /*self*/, PyObject* args) {
  const char* request_data = nullptr;
  Py_ssize_t request_size = 0;
  if (PyArg_ParseTuple(args, "s#", &request_data, &request_size) == 0) return nullptr;

  std::string request(request_data, static_cast<std::size_t>(request_size));
  std::string answer;
  bool failed = false;
  std::string error;

  // Solving can take a while on a large graph, and it touches nothing in the
  // interpreter, so other threads may run meanwhile.
  Py_BEGIN_ALLOW_THREADS
  try {
    answer = gmf::solve_json(request);
  } catch (const std::exception& problem) {
    failed = true;
    error = problem.what();
  } catch (...) {
    failed = true;
    error = "unknown error";
  }
  Py_END_ALLOW_THREADS

  if (failed) {
    PyErr_SetString(PyExc_RuntimeError, error.c_str());
    return nullptr;
  }
  return PyUnicode_FromStringAndSize(answer.data(), static_cast<Py_ssize_t>(answer.size()));
}

PyObject* version(PyObject* /*self*/, PyObject* /*ignored*/) {
  return PyUnicode_FromString(gmf::version());
}

PyMethodDef kMethods[] = {
    {"solve_json", solve_json, METH_VARARGS,
     "solve_json(request: str) -> str\n\n"
     "Solve a generalized maximum flow problem given as a JSON document and\n"
     "return the answer as a JSON document."},
    {"version", version, METH_NOARGS, "version() -> str\n\nThe solver version."},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef kModule = {
    PyModuleDef_HEAD_INIT,
    "_gmaxflow",
    "Generalized maximum flow: the native solver behind the gmaxflow package.",
    -1,
    kMethods,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

}  // namespace

PyMODINIT_FUNC PyInit__gmaxflow(void) { return PyModule_Create(&kModule); }
