FROM ubuntu:22.04

# The C toolchain is kept even though the module is Rust now: the linker for
# the aarch64 target is still gcc's, and cargo drives it rather than replacing
# it.
RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    curl ca-certificates make \
    && rm -rf /var/lib/apt/lists/*

# A pinned toolchain, because a module that rebuilds differently next month is
# a module whose golden render nobody can trust.
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo
ENV PATH=/opt/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
      | sh -s -- -y --default-toolchain 1.98.1 --profile minimal \
        --target aarch64-unknown-linux-gnu \
    && chmod -R a+rwX /opt/cargo /opt/rustup

# cargo must be told which linker the target uses; without it the host's cc is
# invoked on aarch64 objects and the error names neither.
ENV CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-linux-gnu-gcc
ENV CROSS_PREFIX=aarch64-linux-gnu-

WORKDIR /build
