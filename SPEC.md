# Tracer
Restructure the tracer from the ground up for exact and total data flow tracing

A call_id is a per-process serially-assiged 64-bit ID.

Tracked classes are any classes that appear in the vLLM or vLLM-Omni source code or that appear in the tracked.txt config file.

## CPython modifications

PyFrameObject has an additional member `uint64_t call_id`

## Attribute Read/Write Records

Monkey-patch every tracked class to include a `__tr_idx` field as well as a `__arw_{attr}` field for every existing field in the class of type attr_record_write:

struct attr_record_write {
    long caller_id;
    int call_lineno;
} 

Modify __setattr__ to:
* set `__arw_{attr}` with caller_id as the call_id of the parent of __setattr__ and call_lineno as the line in parent where the call to __setattr__ happens
* wrap any builtin container objects that are assigned
* set `database[object.__tr_idx][key]` = `val.__tr_idx` if val has `__tr_idx`

There is also a corrosponding read record that will be used to track data flow from attributes back into functions:

struct attr_record_read {
    long caller_id;
    int write_call_lineno; // corresponding attr_record_write.line_no
    int read_call_lineno;
}

Modify __getattr__ to append to the caller's attr_record_reads

## AST Preprocessing

Parse the AST of every in-scope file. For every function, create a set of all control flow lines (if, elif, while, for)

Also map every co_filename:co_qualname to an integer to be looked up later.

## sys.settrace Hook
Maintain a global counter called next_call_id

On every function call, create a database call record that holds:
* function_id: 32-bit number co_filename:co_qualname maps to
* caller_id: calling function call_id
* call_lineno: line number at which this function was called in its caller
* (if it's a non-static method) obj_id: it's object's call_id
* control_flow_n: 16-bit length field
* control_flow: variable-length bitmap
* attr_record_reads: variable-length array of attr_record_read's

Set PyFrameObject.call_id = next_call_id++

Whenever a control flow line is hit, append "1" to the bitmap if the next executed line is the following line and a 0 otherwise.

On every tracked object instantiation, create a database object record that holds:
* call_id: call_id of the `__init__` invocation that created the object
* members: [string]int map
Then, assign `__tr_idx` the index of the object record in the database.


## Other Class Changes

### Containers
Create a wrapper around each builtin container type (e.g. List, Dict, deque) that maintains an attr_record_write for every contained element.

On set methods, update attr_record_write. On get methods, the function invocation should include an attr_record_read to the accessed container element

These wrappers should be robust to containers that already contain data.

### MessageQueue
Monkey-patch MessageQueue so that __init__ additionally creates a database IPC record that holds:
* name: the shared memory name passed to the constructor
* idx: index in the object record array

# Postprocessor

A separate Python process resolves data dependencies.

Parse the AST for every in-scope file. Then, for each CallRecord, resolve intra-call data flow:
* Establish a list of inbound values: self arguments, attribute reads
* Establish a list of oubound values: self return values, call arguments, attribute writes
* Establish line execution order: control_flow allows you to do this exactly

Remembering to consider the conditional clauses themselves as consumers, you should be able to derive complete and exact data flow for all inbound and outbound values. 

Once all intra-call dependencies have been resolved per-call, merge all call graphs together, saving the named arguments to each call as nodes and the dependences on other named arguments as edges. If a value passes through a member, annotate the edge.
