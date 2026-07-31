from testapp.engine import Engine


def main():
    engine = Engine(workers=3, mode="batch")
    engine.submit("task-a", priority=2)
    engine.submit("task-b", priority=5)
    engine.submit("task-c", priority=1)
    results = engine.run()
    engine.report(results)


if __name__ == "__main__":
    main()
