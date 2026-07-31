from __future__ import annotations

from typing import Any

import sys

from tracer._tracer import Database, IpcRecord, get_call_id


def _record_ipc(db: Database, self_obj: Any) -> None:
    buffer = getattr(self_obj, "buffer", None)
    if buffer is None:
        return
    shm = getattr(buffer, "shared_memory", None)
    if shm is None:
        return
    name = shm.name

    frame = sys._getframe(2)
    caller_id = get_call_id(frame)
    _TAINT = (1 << 64) - 1
    if caller_id == 0 or caller_id == _TAINT:
        return
    db.add_ipc(IpcRecord(name=name, obj_idx=caller_id))


def patch_message_queue(db: Database) -> None:
    try:
        from vllm.distributed.device_communicators.shm_broadcast import (
            MessageQueue,
        )
    except ImportError:
        return

    original_init = MessageQueue.__init__

    def _traced_init(self_obj: Any, *args: Any, **kwargs: Any) -> None:
        original_init(self_obj, *args, **kwargs)
        _record_ipc(db, self_obj)

    MessageQueue.__init__ = _traced_init

    original_from_handle = MessageQueue.create_from_handle

    @staticmethod  # type: ignore
    def _traced_from_handle(*args: Any, **kwargs: Any) -> Any:
        result = original_from_handle(*args, **kwargs)
        _record_ipc(db, result)
        return result

    MessageQueue.create_from_handle = _traced_from_handle
