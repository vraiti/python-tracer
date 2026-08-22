from testapp.core import run
from testapp.ipc import exchange


def main():
    results = run()
    results.update(exchange())
    for key, value in results.items():
        print(f"  {key}: {value}")


if __name__ == "__main__":
    main()
