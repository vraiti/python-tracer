class Task:
    def __init__(self, name: str, priority: int):
        self.name = name
        self.priority = priority
        self.status = "pending"
        self.tags = {}
        self.attempts = []


class Scheduler:
    def __init__(self, capacity: int):
        self.capacity = capacity
        self.queue = []
        self.stats = {"enqueued": 0, "drained": 0}

    def enqueue(self, task):
        if len(self.queue) >= self.capacity:
            self._evict()
        self.queue.append(task)
        self.stats["enqueued"] += 1

    def drain(self):
        sorted_tasks = sorted(self.queue, key=lambda t: t.priority, reverse=True)
        self.queue = []
        self.stats["drained"] += len(sorted_tasks)
        return sorted_tasks

    def _evict(self):
        if self.queue:
            lowest = min(self.queue, key=lambda t: t.priority)
            self.queue.remove(lowest)
