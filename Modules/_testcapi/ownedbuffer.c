#include "parts.h"


// Demonstration of immutable and exclusive buffer ownership mechanism.


typedef struct {
    PyObject_HEAD

    // We _could_ mimic `PyBytes` and inline the buffer here. But we're not
    // really worried about fragmentation in this test.
    char *buf;

    // Number of immutable references exported via the buffer protocol.
    Py_ssize_t immutable_references;

    // Whether the buffer is exclusively exported.
    char exclusively_exported;
} testOwnedBufObject;

static PyObject *
testownedbuf_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    // Allocate a buffer of 1000 bytes that we will expose via the
    // buffer protocol.
    size_t size = 1000;
    char* buf = (char *)PyMem_Calloc(size, sizeof(char));
    if (buf == NULL) {
        return PyErr_NoMemory();
    }

    testOwnedBufObject *self = (testOwnedBufObject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        PyMem_Free(buf);
        return NULL;
    }
    self->buf = buf;
    self->immutable_references = 0;
    self->exclusively_exported = 0;

    return (PyObject *)self;
}

static void
testownedbuf_dealloc(testOwnedBufObject *self)
{
    if (self->exclusively_exported || self->immutable_references > 0) {
        PyErr_SetString(PyExc_SystemError,
                        "deallocated buffer object has exported buffers.");
        PyErr_Print();
    }
    PyMem_Free(self->buf);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static int
testownedbuf_getbuffer(testOwnedBufObject *self, Py_buffer *view, int flags)
{
    // TODO: Do we need to hold the object lock here?

    char exclusive_requested = (flags & PyBUF_EXCLUSIVE) != 0;
    char immutable_requested = (flags & PyBUF_IMMUTABLE) != 0;

    if (self->exclusively_exported) {
        PyErr_SetString(PyExc_BufferError,
                        "Buffer is already exclusively exported.");
        return -1;
    }

    if (flags & PyBUF_EXCLUSIVE && self->immutable_references > 0) {
        PyErr_SetString(PyExc_BufferError,
                        "Buffer has immutable exports and cannot be exclusively exported.");
        return -1;
    }

    if (!(exclusive_requested ^ immutable_requested)) {
        PyErr_SetString(PyExc_BufferError,
                        "exactly one of PyBUF_EXCLUSIVE or PyBUF_IMMUTABLE must be specified.");
        return -1;
    }

    int readonly = exclusive_requested ? 0 : 1;
    if (exclusive_requested) {
        self->exclusively_exported = 1;
    }
    if (immutable_requested) {
        self->immutable_references++;
    }
    return PyBuffer_FillInfo(view, (PyObject *)self, self->buf, 1000, readonly, flags);
}

static void
testownedbuf_releasebufffer(testOwnedBufObject *self, Py_buffer *view)
{
    // TODO: Again, do we need to hold the object lock here?
    assert(self->exclusively_exported ^ (self->immutable_references > 0));
    if (self->exclusively_exported) {
        self->exclusively_exported = 0;
    }
    if (self->immutable_references > 0) {
        self->immutable_references--;
    }
}

static PyBufferProcs testownedbuf_as_buffer = {
    .bf_getbuffer = (getbufferproc) testownedbuf_getbuffer,
    .bf_releasebuffer = (releasebufferproc) testownedbuf_releasebufffer,
    .potential_pybuf_flags = PyBUF_IMMUTABLE | PyBUF_EXCLUSIVE,
};

static PyTypeObject testOwnedBufType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "testOwnedBufType",
    .tp_basicsize = sizeof(testOwnedBufObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = testownedbuf_new,
    .tp_dealloc = (destructor) testownedbuf_dealloc,
    .tp_as_buffer = &testownedbuf_as_buffer,
};

typedef struct {
    PyObject_HEAD

    Py_buffer view;
} testSimpleImmutableViewObject;

// Implement __bytes__ for the immutable view object.
static PyObject *
testSimpleImmutableViewObject___bytes__(testSimpleImmutableViewObject *self)
{
    return PyBytes_FromStringAndSize(self->view.buf, self->view.len);
}

static PyTypeObject testSimpleImmutableViewType;

static PyObject *
testSimpleImmutableViewObject_FromObject(PyObject *base) {
    testSimpleImmutableViewObject *self = (testSimpleImmutableViewObject *)testSimpleImmutableViewType.tp_alloc(&testSimpleImmutableViewType, 0);
    if (self == NULL) {
        return NULL;
    }

    if (PyObject_GetBuffer(base, &self->view, PyBUF_IMMUTABLE) < 0) {
        Py_DECREF(self);
        return NULL;
    }

    // We only support contiguous buffers.
    if (!PyBuffer_IsContiguous(&self->view, 'C')) {
        PyErr_SetString(PyExc_BufferError, "buffer is not contiguous");
        PyBuffer_Release(&self->view);
        Py_DECREF(self);
        return NULL;
    }

    return (PyObject *)self;
}

static PyObject *
testSimpleImmutableViewObject_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    // Extract exactly one argument.
    PyObject *base;
    if (!PyArg_ParseTuple(args, "O", &base)) {
        return NULL;
    }

    return testSimpleImmutableViewObject_FromObject(base);
}

static void
testSimpleImmutableViewObject_dealloc(testSimpleImmutableViewObject *self)
{
    PyBuffer_Release(&self->view);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyMethodDef testSimpleImmutableView_methods[] = {
    {"__bytes__", (PyCFunction) testSimpleImmutableViewObject___bytes__, METH_NOARGS},
    {NULL},
};

static PyTypeObject testSimpleImmutableViewType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "testSimpleImmutableViewType",
    .tp_basicsize = sizeof(testSimpleImmutableViewObject),
    .tp_methods = testSimpleImmutableView_methods,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = testSimpleImmutableViewObject_new,
    .tp_dealloc = (destructor) testSimpleImmutableViewObject_dealloc,
};

// Now an "exclusive" view type.

typedef struct {
    PyObject_HEAD

    Py_buffer view;
} testSimpleMutableViewObject;

static PyObject *
testSimpleMutableViewObject___bytes__(testSimpleMutableViewObject *self)
{
    return PyBytes_FromStringAndSize(self->view.buf, self->view.len);
}

static PyObject *
testSimpleMutableViewObject___setitem__(testSimpleMutableViewObject *self, PyObject *args)
{
    Py_ssize_t index;
    char value;
    if (!PyArg_ParseTuple(args, "nB", &index, &value)) {
        return NULL;
    }

    if (index < 0 || index >= self->view.len) {
        PyErr_SetString(PyExc_IndexError, "index out of range");
        return NULL;
    }

    char* buf = (char *)self->view.buf;
    buf[index] = value;
    Py_RETURN_NONE;
}

static PyTypeObject testSimpleMutableViewType;

static PyObject *
testSimpleMutableViewObject_FromObject(PyObject *base) {
    testSimpleMutableViewObject *self = (testSimpleMutableViewObject *)testSimpleMutableViewType.tp_alloc(&testSimpleMutableViewType, 0);
    if (self == NULL) {
        return NULL;
    }

    if (PyObject_GetBuffer(base, &self->view, PyBUF_EXCLUSIVE) < 0) {
        Py_DECREF(self);
        return NULL;
    }

    // We only support contiguous buffers.
    if (!PyBuffer_IsContiguous(&self->view, 'C')) {
        PyErr_SetString(PyExc_BufferError, "buffer is not contiguous");
        PyBuffer_Release(&self->view);
        Py_DECREF(self);
        return NULL;
    }

    return (PyObject *)self;
}

static PyObject *
testSimpleMutableViewObject_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    // Extract exactly one argument.
    PyObject *base;
    if (!PyArg_ParseTuple(args, "O", &base)) {
        return NULL;
    }

    return testSimpleMutableViewObject_FromObject(base);
}

static void
testSimpleMutableViewObject_dealloc(testSimpleMutableViewObject *self)
{
    PyBuffer_Release(&self->view);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyMethodDef testSimpleMutableView_methods[] = {
    {"__bytes__", (PyCFunction) testSimpleMutableViewObject___bytes__, METH_NOARGS},
    {"__setitem__", (PyCFunction) testSimpleMutableViewObject___setitem__, METH_VARARGS},
    {NULL},
};

static PyTypeObject testSimpleMutableViewType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "testSimpleMutableViewType",
    .tp_basicsize = sizeof(testSimpleMutableViewObject),
    .tp_methods = testSimpleMutableView_methods,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = testSimpleMutableViewObject_new,
    .tp_dealloc = (destructor) testSimpleMutableViewObject_dealloc,
};

int
_PyTestCapi_Init_OwnedBuffer(PyObject *m) {
    if (PyType_Ready(&testOwnedBufType) < 0) {
        return -1;
    }
    if (PyModule_AddObjectRef(m, "testOwnedBuf", (PyObject *)&testOwnedBufType)) {
        return -1;
    }
    // TODO: Move this to a separate function.
    if (PyType_Ready(&testSimpleImmutableViewType) < 0) {
        return -1;
    }
    if (PyModule_AddObjectRef(m, "testSimpleImmutableView", (PyObject *)&testSimpleImmutableViewType)) {
        return -1;
    }
    // TODO: This too.
    if (PyType_Ready(&testSimpleMutableViewType) < 0) {
        return -1;
    }
    if (PyModule_AddObjectRef(m, "testSimpleMutableView", (PyObject *)&testSimpleMutableViewType)) {
        return -1;
    }
    return 0;
}