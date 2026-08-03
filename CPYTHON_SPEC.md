In order for CPython to work with our tracer module, it will need the following patches:

A `_Py_Hashtable_t _PyObjects_Extra` mapping `void*`->`void*` that is initialized at the top of `_freeze_module.c:runtime_init` with allocators malloc and free explicitly.

A private free hook:
`_PyObjects_Extra_Free_hook(void*)`

That is set by a public setter:
`Set_PyObjects_Extra_Free_hook`

A public getter and setter for the hashtable:
`PyObjects_SetExtra(PyObject*,void*)`
`PyObjects_GetExtra(PyObject*)`

These functions print a message to stderr if the caller passes a key that does not exist.

These functions must use the predefined `_Py` function to get the pre-header size and subtract that from the PyObject* to get the correct hashtable key

In PyObject_Malloc, PyObject_Calloc, and PyObject_Realloc, the address of each allocated region is added as a key to `_PyObjects_Extra`.

In PyObject_Free, `_PyObjects_Extra[ptr]` is passed to `_PyObjects_Extra_Free_hook` before being removed as a key from `_PyObjects_Extra`.

Finally, uint64 call_id is added the very first member to `_PyInterpreterFrame`
