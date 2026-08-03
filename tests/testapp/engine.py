from collections import deque

from testapp.scheduler import Scheduler, Task


class Engine:
    def __init__(self, workers: int, mode: str):
        self.workers = workers
        self.mode = mode
        self.config = {"max_retries": 3, "timeout": 30}
        self.scheduler = Scheduler(workers)
        self.results = []
        self.history = deque()

    def submit(self, name: str, priority: int):
        task = Task(name, priority)
        if priority > 3:
            task.tags["urgent"] = True
            self.history.append(("escalated", name))
        else:
            task.tags["routine"] = True
            self.history.append(("queued", name))
        self.scheduler.enqueue(task)

    def run(self):
        results = []
        for task in self.scheduler.drain():
            outcome = self._execute(task)
            results.append(outcome)
            self.results.append(outcome)
        return results

    def _execute(self, task):
        if task.priority > 4:
            return self._fast_path(task)
        else:
            return self._slow_path(task)

    def _fast_path(self, task):
        task.status = "fast"
        retries = self.config.get("max_retries", 1)
        for i in range(retries):
            if i == 0:
                task.attempts.append(("try", i))
                break
            else:
                task.attempts.append(("retry", i))
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
