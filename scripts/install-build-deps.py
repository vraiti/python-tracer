#!/usr/bin/env python3
"""Recursively resolve build dependencies for a pip package via PyPI JSON API.

For each package in the runtime dependency tree, downloads the sdist,
extracts [build-system] requires from pyproject.toml, and installs them.
"""

import json
import os
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

import tomllib

PIP = "/opt/trace-python/bin/pip3"
SDIST_CACHE = Path.home() / ".cache" / "trace-python" / "pip-sdists"
RUN = dict(check=True, stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr)


def pip_cmd() -> list[str]:
    v = int(os.environ.get("PTRACE_PIP_V", "0"))
    return [PIP] + (["-" + "v" * v] if v else [])


def normalize(name: str) -> str:
    return name.lower().replace("-", "_").replace(".", "_")


def strip_extras(dep: str) -> str:
    for ch in ";<>=!@[":
        idx = dep.find(ch)
        if idx != -1:
            dep = dep[:idx]
    return dep.strip()


def get_sdist_url(package: str) -> tuple[str, str] | None:
    url = f"https://pypi.org/pypi/{package}/json"
    try:
        with urllib.request.urlopen(url) as resp:
            data = json.load(resp)
    except urllib.error.HTTPError:
        print(f"  warning: no PyPI entry for {package}")
        return None
    for entry in data.get("urls", []):
        if entry.get("packagetype") == "sdist":
            return entry["url"], entry["filename"]
    return None


def download_sdist(url: str, filename: str) -> Path:
    SDIST_CACHE.mkdir(parents=True, exist_ok=True)
    dest = SDIST_CACHE / filename
    if dest.exists():
        return dest
    print(f"  downloading {filename}")
    urllib.request.urlretrieve(url, dest)
    return dest


def extract_pyproject(archive_path: Path) -> str | None:
    name = archive_path.name
    if name.endswith(".tar.gz") or name.endswith(".tgz"):
        with tarfile.open(archive_path) as tf:
            for member in tf.getmembers():
                parts = member.name.split("/")
                if len(parts) == 2 and parts[1] == "pyproject.toml":
                    f = tf.extractfile(member)
                    if f:
                        return f.read().decode()
    elif name.endswith(".zip"):
        with zipfile.ZipFile(archive_path) as zf:
            for entry in zf.namelist():
                parts = entry.split("/")
                if len(parts) == 2 and parts[1] == "pyproject.toml":
                    return zf.read(entry).decode()
    return None


def collect_deps_from_toml(data: dict, build_deps: list[str], seen_build: set[str],
                           visited: set[str] | None = None, skip: set[str] = frozenset()):
    if visited is None:
        visited = set()
    for dep in data.get("build-system", {}).get("requires", []):
        name = normalize(strip_extras(dep))
        if name not in seen_build and name not in skip:
            seen_build.add(name)
            build_deps.append(dep)
    for dep in data.get("project", {}).get("dependencies", []):
        collect_deps(strip_extras(dep), visited, build_deps, seen_build)


def collect_deps(package: str, visited: set[str], build_deps: list[str], seen_build: set[str]):
    key = normalize(strip_extras(package))
    if key in visited:
        return
    visited.add(key)

    print(f"resolving {package}...")
    info = get_sdist_url(package)
    if not info:
        return
    url, filename = info
    archive = download_sdist(url, filename)

    pyproject = extract_pyproject(archive)
    if not pyproject:
        return
    try:
        data = tomllib.loads(pyproject)
    except Exception as e:
        print(f"  warning: could not parse pyproject.toml of {package}: {e}")
        return

    collect_deps_from_toml(data, build_deps, seen_build, visited=visited, skip={"torch"})

    for dep in data.get("project", {}).get("dependencies", []):
        collect_deps(strip_extras(dep), visited, build_deps, seen_build)


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <package>", file=sys.stderr)
        sys.exit(1)

    package = sys.argv[1]
    build_deps: list[str] = []
    seen_build: set[str] = set()
    collect_deps(package, set(), build_deps, seen_build)

    if not build_deps:
        print(f"No build dependencies found for {package}")
        return

    print(f"\nBuild dependencies found across all packages:")
    for dep in build_deps:
        print(f"  {dep}")

    print(f"\nInstalling {len(build_deps)} build dependencies...")
    subprocess.run([
        *pip_cmd(), "install",
        "--no-binary", ":all:",
        "--cache-dir", str(SDIST_CACHE),
    ] + build_deps, **RUN)


if __name__ == "__main__":
    main()
