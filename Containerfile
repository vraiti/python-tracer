# Build environment matching the RHEL 10 deploy target's glibc (2.39) --
# building on a newer host (e.g. this dev laptop) links against a newer
# glibc that the RHEL 10 target can't satisfy at runtime (confirmed:
# `GLIBC_2.42' not found` for termios.cpython-314-x86_64-linux-gnu.so when
# a build done outside this container was deployed there).
FROM registry.access.redhat.com/ubi10/ubi:latest

# readline-devel and gdbm-devel aren't published in UBI10's public repos
# (only the runtime readline/gdbm packages are, not their -devel headers --
# those require a full RHEL subscription). Both are optional CPython
# extension modules (readline: interactive REPL editing; _gdbm: dbm.gnu),
# so the build just skips them rather than failing.
RUN dnf install -y \
        gcc gcc-c++ make git \
        zlib-devel bzip2-devel xz-devel libffi-devel \
        openssl-devel sqlite-devel \
        tk-devel libuuid-devel expat-devel \
        ncurses-devel \
    && dnf clean all

RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
ENV PATH="/root/.cargo/bin:${PATH}"

WORKDIR /src
CMD ["make", "build"]
