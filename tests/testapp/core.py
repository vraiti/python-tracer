"""Single-process coverage: calls, control flow, objects, containers,
closures, globals, threads, and scope exclusion."""

import threading
from collections import deque

counter = 0        # global load/store
transient = None   # global delete


class Payload:
    def __init__(self, label):
        self.label = label
        self.history = []      # list  -> append (c_call)
        self.meta = {}         # dict  -> setitem / getitem
        self.seen = set()      # set   -> add (c_call)
        self.queue = deque()   # deque -> append / popleft (c_call)
        self.shape = (1, 2)    # tuple -> getitem (read-only)

    def record(self, value):
        if value < 0:
            self.meta["sign"] = "neg"
        elif value == 0:
            self.meta["sign"] = "zero"
        else:
            self.meta["sign"] = "pos"
        self.history.append(value)
        self.seen.add(value)
        self.queue.append(value)
        return self.meta["sign"]

    def drain(self):
        total = 0
        while self.queue:
            total += self.queue.popleft()
        return total * self.shape[1]


class Node:
    def __init__(self, payload):
        self.payload = payload          # object -> object member
        self.peer = None                # object member, cycle target
        self.nested = {"inner": []}     # container inside container

    def link(self, other):
        self.peer = other
        other.peer = self               # cycle -> default_owner must break it


def make_accumulator():
    total = 0

    def add(n):
        nonlocal total                  # deref store
        total += n
        return total                    # deref load

    return add


async def describe(tag):
    return "cell:" + tag


async def relay(fn):
    return await fn()               # indirect call of a received closure


def make_task(tag):
    # `tag` is a captured parameter: it enters its cell via MAKE_CELL,
    # never through STORE_DEREF.
    return lambda: describe(tag)


def decoy():
    return "-decoy"


async def relay_arg(fn):
    # `decoy()` executes (and is recorded) before the lambda, so pairing
    # `fn` with its child requires identity, not queue position.
    return await fn(decoy())


def scratch_noise(payload):
    """Excluded via taint-functions; nothing beneath this call is traced."""
    payload.record(99)


def safe_div(a, b):
    try:
        return a / b
    except ZeroDivisionError:
        return None


def poll(attempts):
    """A handler entered on some iterations: exception-driven control flow
    the replay has to follow through the recorded handler entries."""
    got = 0
    for i in range(attempts):
        try:
            got += safe_index([0, 1], i)
        except IndexError:
            got -= 1
    return got


def safe_index(seq, i):
    return seq[i]


async def ticks(n):
    for i in range(n):
        yield i


async def consume(n, flag):
    """Control flow the statement skeleton cannot replay: a compound
    condition, a ternary, an inlined comprehension and an async for."""
    kind = "some" if n > 1 else "none"
    squares = [i * i for i in range(n) if i % 2 == 0]
    acc = 0
    async for i in ticks(n):
        if flag and i % 2 == 1:
            acc += i
        else:
            acc -= 1
    return {"kind": kind, "squares": squares, "acc": acc}


def run():
    global counter, transient

    shared = Payload("shared")
    left, right = Node(shared), Node(shared)   # two parents for one child
    left.link(right)
    left.nested["inner"].append(shared.label)  # mutate a setitem-attached child

    for value in (3, -1, 0):
        shared.record(value)
        counter += 1

    acc = make_accumulator()
    for i in range(3):
        acc(i)

    worker = threading.Thread(target=shared.record, args=(7,))
    worker.start()
    worker.join()

    scratch_noise(shared)
    transient = safe_div(1, 0)
    del transient
    polled = poll(4)

    import asyncio
    consumed = asyncio.run(consume(4, True))
    described = asyncio.run(relay(make_task("qi")))
    suffixed = asyncio.run(relay_arg(lambda s: describe("arg" + s)))

    return {
        "consumed": consumed,
        "described": described,
        "polled": polled,
        "drained": shared.drain(),
        "sign": shared.meta["sign"],
        "acc": acc(0),
        "counter": counter,
        "peer_ok": left.peer.peer is left,
    }
