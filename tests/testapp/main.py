from testapp.engine import Engine
from testapp.ipc_test import run_all as run_ipc_tests


def main():
    engine = Engine(workers=3, mode="batch")
    engine.submit("task-a", priority=2)
    engine.submit("task-b", priority=5)
    engine.submit("task-c", priority=1)
    results = engine.run()
    engine.report(results)

    ipc_results = run_ipc_tests()
    for name, result in ipc_results.items():
        print(f"  {name}: {result}")


if __name__ == "__main__":
    main()
