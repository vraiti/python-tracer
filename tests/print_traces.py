import sys
from collections import deque


def prof(frame, event, arg):
    if event in ("c_call", "c_return"):
        print(f"{event}: {arg!r}")


sys.setprofile(prof)

# list methods
mylist = [1, 2, 3]
mylist.append(4)
mylist.extend([5, 6])
mylist.insert(0, 0)
mylist.pop()
mylist.remove(0)
mylist.clear()

# dict methods
mydict = {"a": 1}
mydict["b"] = 2
mydict.update({"c": 3})
mydict.setdefault("d", 4)
mydict.pop("a")
mydict.get("b")
mydict.clear()

# set methods
myset = {1, 2, 3}
myset.add(4)
myset.discard(2)
myset.remove(3)
myset.pop()
myset.update({5, 6})
myset.clear()

# deque methods
mydq = deque([1, 2, 3])
mydq.append(4)
mydq.appendleft(0)
mydq.extend([5, 6])
mydq.extendleft([7, 8])
mydq.pop()
mydq.popleft()
mydq.remove(1)
mydq.clear()

sys.setprofile(None)
