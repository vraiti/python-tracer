"""Cross-process coverage: every IPC and IO hook, through one fork.

Ordering is chosen so neither side can deadlock: each blocking rendezvous
(fifo open, accept, semaphore) is preceded on the other side by a
non-blocking step that guarantees the peer has reached it.
"""

import mmap
import multiprocessing
import os
import signal
import socket
import tempfile
from multiprocessing import shared_memory

got_signal = False


def _on_usr1(signum, frame):
    global got_signal
    got_signal = True


def exchange():
    tmp = tempfile.mkdtemp()
    fifo_path = os.path.join(tmp, "fifo")
    sock_path = os.path.join(tmp, "sock")
    data_path = os.path.join(tmp, "data")
    map_path = os.path.join(tmp, "map")

    os.mkfifo(fifo_path)                                        # mkfifo
    r, w = os.pipe()                                            # pipe
    shm = shared_memory.SharedMemory(create=True, size=8)       # shm_open
    sem = multiprocessing.Semaphore(0)                          # semaphore
    srv = socket.socket(socket.AF_UNIX)
    srv.bind(sock_path)                                         # socket bind
    srv.listen(1)
    signal.signal(signal.SIGUSR1, _on_usr1)                     # signal
    with open(map_path, "wb") as f:                             # fileio open/write
        f.write(b"\0" * 8)
    map_fd = os.open(map_path, os.O_RDWR)
    region = mmap.mmap(map_fd, 8)                               # mmap create

    pid = os.fork()                                             # after_fork_child
    if pid == 0:
        os.write(w, b"pipe")
        with open(fifo_path, "w") as f:
            f.write("fifo")
        child_shm = shared_memory.SharedMemory(name=shm.name, track=False)
        child_shm.buf[:3] = b"shm"
        child_shm.close()
        client = socket.socket(socket.AF_UNIX)
        client.connect(sock_path)                               # socket connect
        client.sendall(b"sock")
        client.close()
        child_map = mmap.mmap(os.open(map_path, os.O_RDWR), 8)
        child_map[:3] = b"map"                                  # mmap write
        child_map.close()
        with open(data_path, "w") as f:
            f.write("file")
        os.kill(os.getppid(), signal.SIGUSR1)
        sem.release()
        os._exit(0)

    out = {"pipe": os.read(r, 4)}
    with open(fifo_path) as f:
        out["fifo"] = f.read()
    conn, _ = srv.accept()
    out["sock"] = conn.recv(4)
    conn.close()
    sem.acquire()
    out["shm"] = bytes(shm.buf[:3])
    out["map"] = region[:3]                                     # mmap read
    with open(data_path) as f:                                  # fileio read
        out["file"] = f.read()
    out["signal"] = got_signal
    os.waitpid(pid, 0)

    region.close()
    os.close(map_fd)
    shm.close()
    shm.unlink()
    srv.close()
    return out
