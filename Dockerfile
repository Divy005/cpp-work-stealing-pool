# Reproducible Linux build environment for cpp-work-stealing-pool.
#
# Provides:
#   * Release build + all tests (default)
#   * ThreadSanitizer gate (zero data races)
#   * AddressSanitizer + UBSan gate (zero memory bugs, zero leaks)
#   * Benchmark and evaluation harness
#
# Usage:
#   docker build -t wsp .
#   docker run --rm wsp                          # build + test (Release)
#   docker run --rm wsp make tsan                # TSan gate
#   docker run --rm wsp make asan                # ASan+UBSan gate
#   docker run --rm wsp make bench               # micro-benchmark
#   docker run --rm wsp make eval                # evaluation harness

FROM ubuntu:24.04

# Avoid interactive prompts during package installation.
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        ca-certificates \
        wget \
        && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# The top-level Makefile drives the container entry-point. Build it here so the
# image already contains the compiled binaries (faster iteration when re-running
# the container without rebuilding the image).
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) \
    && ctest --test-dir build --output-on-failure

# Create a thin Makefile that the docker run entry-point delegates to.  This
# lets callers type `docker run --rm wsp make tsan` instead of a long cmake
# command.
RUN cat > /Makefile <<'MAKE'
.PHONY: test tsan asan bench eval scaling

test:
	cmake -S /src -B /build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build /build-rel -j$(shell nproc)
	ctest --test-dir /build-rel --output-on-failure

tsan:
	cmake -S /src -B /build-tsan -G Ninja \
	    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	    -DWSP_SANITIZER=thread
	cmake --build /build-tsan -j$(shell nproc)
	TSAN_OPTIONS="halt_on_error=1" /build-tsan/wsp_tests
	@echo "=== TSan: CLEAN ==="

asan:
	cmake -S /src -B /build-asan -G Ninja \
	    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	    -DWSP_SANITIZER=address,undefined
	cmake --build /build-asan -j$(shell nproc)
	ASAN_OPTIONS="halt_on_error=1:detect_leaks=1" /build-asan/wsp_tests
	@echo "=== ASan+UBSan: CLEAN ==="

bench:
	/src/build/wsp_bench

eval:
	/src/build/wsp_eval --pool both

scaling:
	/src/build/wsp_eval --scaling
MAKE

CMD ["ctest", "--test-dir", "/src/build", "--output-on-failure"]
