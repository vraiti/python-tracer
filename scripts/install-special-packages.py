#!/usr/bin/env python3
"""Install packages that don't publish sdists on PyPI.

Recurses through runtime dependencies via the PyPI JSON API.
If torch is found, clones it from git and installs with pip install -e.
"""

import importlib.util
import json
import os
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

import tomllib

_spec = importlib.util.spec_from_file_location(
    "install_build_deps", Path(__file__).parent / "install-build-deps.py",
)
install_build_deps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(install_build_deps)

PIP = "/opt/trace-python/bin/pip3"
GIT_REPOS = Path.home() / ".cache" / "trace-python" / "git-repos"
SDIST_CACHE = Path.home() / ".cache" / "trace-python" / "pip-sdists"
RUN = dict(check=True, stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr)

SPECIAL = {
    "torch": {
        "repo": "https://github.com/pytorch/pytorch.git",
        "dir": "pytorch",
    },
}


def normalize(name: str) -> str:
    return name.lower().replace("-", "_").replace(".", "_")


def strip_extras(dep: str) -> str:
    for ch in ";<>=!@[":
        idx = dep.find(ch)
        if idx != -1:
            dep = dep[:idx]
    return dep.strip()


def extract_version(dep: str) -> str | None:
    for prefix in ["==", "~="]:
        idx = dep.find(prefix)
        if idx != -1:
            ver = dep[idx + len(prefix):].strip()
            for ch in ",; ":
                end = ver.find(ch)
                if end != -1:
                    ver = ver[:end]
            return ver
    return None


def get_pyproject_from_pypi(package: str) -> dict | None:
    url = f"https://pypi.org/pypi/{package}/json"
    try:
        with urllib.request.urlopen(url) as resp:
            data = json.load(resp)
    except urllib.error.HTTPError:
        return None

    for entry in data.get("urls", []):
        if entry.get("packagetype") == "sdist":
            filename = entry["filename"]
            dest = SDIST_CACHE / filename
            if not dest.exists():
                SDIST_CACHE.mkdir(parents=True, exist_ok=True)
                urllib.request.urlretrieve(entry["url"], dest)

            if filename.endswith(".tar.gz") or filename.endswith(".tgz"):
                with tarfile.open(dest) as tf:
                    for member in tf.getmembers():
                        parts = member.name.split("/")
                        if len(parts) == 2 and parts[1] == "pyproject.toml":
                            f = tf.extractfile(member)
                            if f:
                                try:
                                    return tomllib.loads(f.read().decode())
                                except Exception:
                                    return None
    return None


def find_special(package: str, dep_spec: str, visited: set[str], found: dict[str, str]):
    key = normalize(strip_extras(package))
    if key in visited:
        return
    visited.add(key)

    if key in SPECIAL:
        version = extract_version(dep_spec)
        found[key] = version
        return

    data = get_pyproject_from_pypi(package)
    if not data:
        return

    for dep in data.get("build-system", {}).get("requires", []):
        find_special(strip_extras(dep), dep, visited, found)
    for dep in data.get("project", {}).get("dependencies", []):
        find_special(strip_extras(dep), dep, visited, found)


def install_torch(version: str | None):
    repo_dir = GIT_REPOS / "pytorch"
    GIT_REPOS.mkdir(parents=True, exist_ok=True)

    tag = f"v{version}" if version else "main"

    if repo_dir.exists():
        print(f"pytorch repo exists, fetching tag {tag}...")
        subprocess.run(
            ["git", "-C", str(repo_dir), "fetch", "--depth=1", "origin", f"refs/tags/{tag}:refs/tags/{tag}"],
            **RUN,
        )
        subprocess.run(
            ["git", "-C", str(repo_dir), "checkout", tag],
            **RUN,
        )
    else:
        print(f"cloning pytorch at {tag}...")
        subprocess.run(
            ["git", "clone", "--depth=1", "--branch", tag,
             "https://github.com/pytorch/pytorch.git", str(repo_dir)],
            **RUN,
        )

    subprocess.run(
        ["git", "-C", str(repo_dir), "submodule", "sync", "--recursive"],
        **RUN,
    )
    subprocess.run(
        ["git", "-C", str(repo_dir), "submodule", "update", "--init", "--recursive", "--force", "--depth=1"],
        **RUN,
    )

    print("installing pytorch build dependencies from source...")
    pyproject_path = repo_dir / "pyproject.toml"
    with open(pyproject_path, "rb") as f:
        data = tomllib.load(f)
    build_deps: list[str] = []
    seen_build: set[str] = set()
    install_build_deps.collect_deps_from_toml(data, build_deps, seen_build)

    skip_dirs = {"docs", "test", "dev"}
    visited = set()
    for req_file in repo_dir.rglob("requirements*.txt"):
        if any(part for part in req_file.parts if any(s in part for s in skip_dirs)):
            continue
        print(f"  scanning {req_file.relative_to(repo_dir)}")
        for line in req_file.read_text().splitlines():
            line = line.strip()
            if "#" in line:
                line = line[:line.index("#")].strip()
            if not line or line.startswith("-"):
                continue
            pkg = install_build_deps.strip_extras(line)
            install_build_deps.collect_deps(pkg, visited, build_deps, seen_build)

    if build_deps:
        print(f"  torch build deps: {build_deps}")
        subprocess.run([
            *install_build_deps.pip_cmd(), "install",
            "--no-binary", ":all:",
            "--cache-dir", str(SDIST_CACHE),
        ] + build_deps, **RUN)

    os.environ["USE_NNPACK"] = "0"
    os.environ["USE_DISTRIBUTED"] = "0"
    os.environ["USE_NCCL"] = "0"
    os.environ["USE_TENSORPIPE"] = "0"
    os.environ["USE_GLOO"] = "0"
    os.environ["USE_XNNPACK"] = "0"
    os.environ["USE_FBGEMM"] = "0"
    os.environ["USE_QNNPACK"] = "0"
    os.environ["USE_KINETO"] = "0"
    os.environ["BUILD_TEST"] = "0"
    os.environ["BUILD_CAFFE2"] = "0"

    print("installing pytorch...")
    v = int(os.environ.get("PTRACE_PIP_V", "0"))
    v_flag = ["-" + "v" * v] if v else []
    subprocess.run([PIP, *v_flag, "install", "--no-binary", ":all:", "--no-build-isolation", "-e", str(repo_dir)], **RUN)


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <package>", file=sys.stderr)
        sys.exit(1)

    package = sys.argv[1]
    found: dict[str, str] = {}
    find_special(package, package, set(), found)

    if not found:
        print("No special packages found in dependency tree.")
        return

    for name, version in found.items():
        print(f"found {name} == {version}")
        if name == "torch":
            install_torch(version)


if __name__ == "__main__":
    main()
