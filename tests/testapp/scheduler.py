total_tasks_created = 0
total_tasks_drained = 0


class Task:
    def __init__(self, name: str, priority: int):
        global total_tasks_created
        self.name = name
        self.priority = priority
        self.status = "pending"
        self.tags = {}
        self.attempts = []
        total_tasks_created += 1


def make_priority_filter(threshold):
    filtered_count = 0

    def apply(tasks):
        nonlocal filtered_count
        result = []
        for t in tasks:
            if t.priority >= threshold:
                result.append(t)
                filtered_count += 1
        return result, filtered_count

    return apply


class Scheduler:
    def __init__(self, capacity: int):
        self.capacity = capacity
        self.queue = []
        self.stats = {"enqueued": 0, "drained": 0}
        self.priority_filter = make_priority_filter(2)

    def enqueue(self, task):
        if len(self.queue) >= self.capacity:
            self._evict()
        self.queue.append(task)
        self.stats["enqueued"] += 1

    def drain(self):
        global total_tasks_drained
        filtered, count = self.priority_filter(self.queue)
        sorted_tasks = sorted(filtered, key=lambda t: t.priority, reverse=True)
        self.queue = []
        self.stats["drained"] += len(sorted_tasks)
        total_tasks_drained += len(sorted_tasks)
        return sorted_tasks

    def _evict(self):
        if self.queue:
            lowest = min(self.queue, key=lambda t: t.priority)
            self.queue.remove(lowest)
