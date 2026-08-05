import mmap
import multiprocessing
import multiprocessing.shared_memory
import os
import signal
import socket
import struct
import tempfile


def test_pipe_relay():
    """Parent writes to pipe, child reads and writes result to a second pipe."""
    r1, w1 = os.pipe()
    r2, w2 = os.pipe()

    pid = os.fork()
    if pid == 0:
        os.close(w1)
        os.close(r2)
        data = os.read(r1, 64)
        value = int(data) * 2
        os.write(w2, str(value).encode())
        os.close(r1)
        os.close(w2)
        os._exit(0)

    os.close(r1)
    os.close(w2)
    os.write(w1, b"21")
    os.close(w1)
    result = os.read(r2, 64)
    os.close(r2)
    os.waitpid(pid, 0)
    return int(result)


def test_named_pipe():
    """Two processes communicate through a named pipe (FIFO)."""
    fifo_path = os.path.join(tempfile.mkdtemp(), "test.fifo")
    os.mkfifo(fifo_path)

    pid = os.fork()
    if pid == 0:
        with open(fifo_path, "w") as f:
            f.write("hello from child")
        os._exit(0)

    with open(fifo_path, "r") as f:
        data = f.read()
    os.waitpid(pid, 0)
    os.unlink(fifo_path)
    return data


def test_unix_socket():
    """Parent and child exchange messages over a Unix domain socket pair."""
    parent_sock, child_sock = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)

    pid = os.fork()
    if pid == 0:
        parent_sock.close()
        msg = child_sock.recv(64)
        child_sock.sendall(msg.upper())
        child_sock.close()
        os._exit(0)

    child_sock.close()
    parent_sock.sendall(b"ping")
    reply = parent_sock.recv(64)
    parent_sock.close()
    os.waitpid(pid, 0)
    return reply


def test_tcp_socket():
    """Child connects to parent's TCP server and sends data."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    srv.listen(1)

    pid = os.fork()
    if pid == 0:
        srv.close()
        c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        c.connect(("127.0.0.1", port))
        c.sendall(b"payload")
        c.close()
        os._exit(0)

    conn, _ = srv.accept()
    data = conn.recv(64)
    conn.close()
    srv.close()
    os.waitpid(pid, 0)
    return data


def test_shared_memory():
    """Two processes share data through POSIX shared memory."""
    shm = multiprocessing.shared_memory.SharedMemory(create=True, size=64)

    pid = os.fork()
    if pid == 0:
        child_shm = multiprocessing.shared_memory.SharedMemory(name=shm.name)
        child_shm.buf[:5] = b"child"
        child_shm.close()
        os._exit(0)

    os.waitpid(pid, 0)
    result = bytes(shm.buf[:5])
    shm.close()
    shm.unlink()
    return result


def test_mmap_file():
    """Two processes communicate through a memory-mapped file."""
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(b"\x00" * 4096)
        path = f.name

    fd = os.open(path, os.O_RDWR)
    m = mmap.mmap(fd, 4096)

    pid = os.fork()
    if pid == 0:
        m[0:6] = b"mapped"
        m.flush()
        m.close()
        os.close(fd)
        os._exit(0)

    os.waitpid(pid, 0)
    result = m[0:6]
    m.close()
    os.close(fd)
    os.unlink(path)
    return result


def test_mmap_anonymous():
    """Parent and child share an anonymous mmap region."""
    m = mmap.mmap(-1, 4096)
    m.write(b"\x00" * 8)

    pid = os.fork()
    if pid == 0:
        m.seek(0)
        m.write(struct.pack("Q", 42))
        m.close()
        os._exit(0)

    os.waitpid(pid, 0)
    m.seek(0)
    value = struct.unpack("Q", m.read(8))[0]
    m.close()
    return value


def test_file_io():
    """Parent writes a file, child reads it."""
    with tempfile.NamedTemporaryFile(mode="w", delete=False, suffix=".dat") as f:
        f.write("file_data_from_parent")
        path = f.name

    pid = os.fork()
    if pid == 0:
        with open(path, "r") as f:
            data = f.read()
        with open(path + ".out", "w") as f:
            f.write(data.upper())
        os._exit(0)

    os.waitpid(pid, 0)
    with open(path + ".out", "r") as f:
        result = f.read()
    os.unlink(path)
    os.unlink(path + ".out")
    return result


def test_semaphore():
    """Semaphore coordinates producer-consumer between two processes."""
    sem = multiprocessing.Semaphore(0)
    r, w = os.pipe()

    pid = os.fork()
    if pid == 0:
        os.close(r)
        os.write(w, b"ready")
        os.close(w)
        sem.release()
        os._exit(0)

    os.close(w)
    sem.acquire()
    data = os.read(r, 64)
    os.close(r)
    os.waitpid(pid, 0)
    return data


_signal_received = False


def test_signal():
    """Child sends a signal to parent."""
    global _signal_received

    def handler(signum, frame):
        global _signal_received
        _signal_received = True

    old = signal.signal(signal.SIGUSR1, handler)

    pid = os.fork()
    if pid == 0:
        os.kill(os.getppid(), signal.SIGUSR1)
        os._exit(0)

    os.waitpid(pid, 0)
    signal.signal(signal.SIGUSR1, old)
    return _signal_received


def test_mmap_operations():
    """Exercise all mmap read/write operations: index, slice, methods."""
    m = mmap.mmap(-1, 256)

    m.write(b"hello world")
    m.seek(0)
    data = m.read(5)

    m.write_byte(ord("!"))
    m.seek(6)
    b = m.read_byte()

    m[100] = 0xFF
    val = m[100]

    m[200:205] = b"slice"
    s = m[200:205]

    m.seek(0)
    line = m.readline()

    m.move(50, 0, 11)

    m.seek(0)
    pos = m.find(b"world")

    m.close()
    return {
        "read": data,
        "write_byte": chr(b),
        "index": val,
        "slice": s,
        "readline": line,
        "find_pos": pos,
    }


def run_all():
    results = {}
    tests = [
        ("pipe_relay", test_pipe_relay),
        ("named_pipe", test_named_pipe),
        ("unix_socket", test_unix_socket),
        ("tcp_socket", test_tcp_socket),
        ("shared_memory", test_shared_memory),
        ("mmap_file", test_mmap_file),
        ("mmap_anonymous", test_mmap_anonymous),
        ("mmap_operations", test_mmap_operations),
        ("file_io", test_file_io),
        ("semaphore", test_semaphore),
        ("signal", test_signal),
    ]
    for name, fn in tests:
        try:
            results[name] = fn()
        except Exception as e:
            results[name] = f"FAILED: {e}"
    return results
