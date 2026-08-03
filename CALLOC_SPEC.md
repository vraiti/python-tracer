PyObject_Calloc is used to allocate members of container-types (which our tracer wants to trace using the code in csrc/containers/). This creates problems for our tracer since it needs to be able to free `_PyObject_Extra` values, but neither PyObject_Free nor our free hook can tell natively whether the region was calloc'd.

Add a `_Py_Hashtable_t _Py_Calloc_Addrs` parallel to `_PyObjects_Extra`. When `PyObject_Calloc` is called, it simply saves `_Py_Calloc_Adds[ptr] = {nelem, elsize}`.

Additionally, create nelem `_PyObject_Extra` members with stride `elsize`:
```c
for ( void *cur=ptr; ptr < ptr + nelem * elsize; ptr += elsize )
    _PyObjects_Extra[cur] = NULL;
```

When PyObject_Free is called:
```c
value = &_Py_Calloc_Addrs[ptr];
if (nelem_addr) {
    for (void *cur=ptr; ptr < ptr + value->nelem*value->elsize; ptr += value->elsize)
        free_hook(cur);
}
```
