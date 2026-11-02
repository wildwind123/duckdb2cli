FROM mcr.azure.cn/devcontainers/base:trixie

ARG DUCKDB_VERSION=2.0.0

# C toolchain (plus MinGW-w64 for Windows cross-builds), build tools, debugging and analysis
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        clang-format \
        clang-tidy \
        cmake \
        ninja-build \
        pkg-config \
        ccache \
        gdb \
        valgrind \
        libreadline-dev \
        gcc-mingw-w64-x86-64 \
        curl \
        ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# DuckDB C library (libduckdb.so + headers) and the duckdb CLI for reference.
# Pinned version, so this layer stays in the Docker build cache across rebuilds.
RUN set -eux; \
    case "$(dpkg --print-architecture)" in \
        amd64) arch=amd64 ;; \
        arm64) arch=arm64 ;; \
        *) echo "unsupported arch" >&2; exit 1 ;; \
    esac; \
    base="https://github.com/duckdb/duckdb/releases/download/v${DUCKDB_VERSION}"; \
    tmp="$(mktemp -d)"; \
    curl -fsSL "${base}/duckdb-shared-libs-linux-${arch}.tar.gz" | tar -xz -C "$tmp"; \
    install -m 0644 "$tmp"/*.h /usr/local/include/; \
    install -m 0755 "$tmp"/libduckdb.so /usr/local/lib/; \
    curl -fsSL "${base}/duckdb-cli-linux-${arch}.tar.gz" | tar -xz -C /usr/local/bin duckdb; \
    rm -rf "$tmp"; \
    ldconfig

# Use ccache for gcc/clang transparently
ENV PATH=/usr/lib/ccache:${PATH}

# Mount targets must exist and belong to vscode, otherwise volumes are created root-owned
RUN mkdir -p /home/vscode/.vscode-server /home/vscode/.claude /home/vscode/.cache/ccache /home/vscode/.duckdb && \
    chown -R vscode:vscode /home/vscode/.vscode-server /home/vscode/.claude /home/vscode/.cache /home/vscode/.duckdb

COPY no-proxy-host.sh /etc/profile.d/20-no-proxy-host.sh
RUN echo '. /etc/profile.d/20-no-proxy-host.sh' >> /etc/bash.bashrc

USER vscode
