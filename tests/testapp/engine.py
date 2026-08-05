from collections import deque

from testapp.scheduler import Scheduler, Task, total_tasks_created, total_tasks_drained

last_engine_status = None


def make_retry_strategy(max_retries):
    attempt_log = []

    def execute(task):
        nonlocal attempt_log
        for i in range(max_retries):
            attempt_log.append((task.name, i))
            if i == 0:
                task.attempts.append(("try", i))
                return True
            task.attempts.append(("retry", i))
        return False

    def get_log():
        return list(attempt_log)

    return execute, get_log


class Engine:
    def __init__(self, workers: int, mode: str):
        global last_engine_status
        self.workers = workers
        self.mode = mode
        self.config = {"max_retries": 3, "timeout": 30}
        self.scheduler = Scheduler(workers)
        self.results = []
        self.history = deque()
        self.retry, self.get_retry_log = make_retry_strategy(
            self.config["max_retries"]
        )
        last_engine_status = "initialized"

    def submit(self, name: str, priority: int):
        global last_engine_status
        task = Task(name, priority)
        if priority > 3:
            task.tags["urgent"] = True
            self.history.append(("escalated", name))
        else:
            task.tags["routine"] = True
            self.history.append(("queued", name))
        self.scheduler.enqueue(task)
        last_engine_status = "submitting"

    def run(self):
        global last_engine_status
        last_engine_status = "running"
        results = []
        for task in self.scheduler.drain():
            outcome = self._execute(task)
            results.append(outcome)
            self.results.append(outcome)
        last_engine_status = "finished"
        return results

    def _execute(self, task):
        if task.priority > 4:
            return self._fast_path(task)
        else:
            return self._slow_path(task)

    def _fast_path(self, task):
        task.status = "fast"
        self.retry(task)
        return {"task": task.name, "path": "fast", "attempts": len(task.attempts)}

    def _slow_path(self, task):
        task.status = "slow"
        timeout = self.config.get("timeout", 10)
        if timeout > 20:
            task.attempts.append(("extended", timeout))
        else:
            task.attempts.append(("standard", timeout))
        return {"task": task.name, "path": "slow", "attempts": len(task.attempts)}

    def report(self, results):
        summary = {}
        for r in results:
            path = r["path"]
            if path in summary:
                summary[path] += 1
            else:
                summary[path] = 1
        self.config["last_run"] = summary
        self.config["retry_log"] = self.get_retry_log()
        self.config["total_created"] = total_tasks_created
        self.config["total_drained"] = total_tasks_drained
