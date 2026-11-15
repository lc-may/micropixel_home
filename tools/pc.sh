#!/usr/bin/env bash
set -euo pipefail
pc_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$pc_root"
pc_deps="$pc_root/build/pc-deps"
pc_build="$pc_root/build/pc"

pc_clone() {
    local directory="$1" url="$2" revision="$3"
    if [[ ! -d "$directory/.git" && ! -f "$directory/.git" ]]; then
        git clone --depth 1 --branch "$revision" "$url" "$directory"
    fi
    local actual
    actual="$(git -C "$directory" rev-parse HEAD)"
    case "$revision" in
        v9.6.0) [[ "$actual" == 80ca777e37a2b176770726a02e07a6fb79ef0b39 ]] ;;
        v0.4.1) [[ "$actual" == 8354085908683c6130e32a832aeec8a7ca115c51 ]] ;;
        v2.3.1) [[ "$actual" == 2beef04725328923e05e576b884212d53ec97af7 ]] ;;
        v3.1.1) [[ "$actual" == 12adee81ddef588abada91672b34fd997b2a27ac ]] ;;
    esac || { echo "Unexpected dependency revision: $directory ($actual)" >&2; exit 2; }
}
pc_configure() {
    local args=()
    if [[ -x "$pc_deps/compiler/root/usr/bin/g++-12" ]]; then
        args+=("-DCMAKE_C_COMPILER=$pc_deps/compiler/root/usr/bin/gcc-12"
               "-DCMAKE_CXX_COMPILER=$pc_deps/compiler/root/usr/bin/g++-12")
    fi
    cmake -S firmware/pc -B "$pc_build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "${args[@]}"
}
pc_rv32_build() {
    local pc_rv32_cc="${MICROPIXEL_RV32_CC:-}"
    if [[ -z "$pc_rv32_cc" ]]; then
        pc_rv32_cc="$(command -v riscv64-unknown-elf-gcc || command -v riscv32-unknown-elf-gcc || true)"
    fi
    if [[ -z "$pc_rv32_cc" && -x "$HOME/bouffalo_sdk/toolchain_gcc_t-head_linux/bin/riscv64-unknown-elf-gcc" ]]; then
        pc_rv32_cc="$HOME/bouffalo_sdk/toolchain_gcc_t-head_linux/bin/riscv64-unknown-elf-gcc"
    fi
    if [[ -z "$pc_rv32_cc" ]]; then
        echo 'Set MICROPIXEL_RV32_CC to a Newlib GCC with rv32imafc/ilp32f multilib support.' >&2
        exit 2
    fi
    MICROPIXEL_RV32_CC="$pc_rv32_cc" cmake -S firmware/pc/rv32 -B build/pc-rv32 -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$pc_root/firmware/pc/rv32/toolchain.cmake"
    cmake --build build/pc-rv32 --parallel "${MICROPIXEL_PC_JOBS:-4}"
}
case "${1:-help}" in
    deps)
        mkdir -p "$pc_deps/compiler"
        git submodule update --init --depth 1 firmware/espressif/components/wasm-micro-runtime
        pc_clone "$pc_deps/lvgl" https://github.com/lvgl/lvgl.git v9.6.0
        pc_clone "$pc_deps/micro-opus" https://github.com/esphome-libs/micro-opus.git v0.4.1
        git -C "$pc_deps/micro-opus" submodule update --init --depth 1 lib/micro-ogg-demuxer
        pc_clone "$pc_deps/coreMQTT" https://github.com/FreeRTOS/coreMQTT.git v2.3.1
        pc_clone "$pc_deps/coreHTTP" https://github.com/FreeRTOS/coreHTTP.git v3.1.1
        git -C "$pc_deps/coreHTTP" submodule update --init --depth 1 source/dependency/3rdparty/llhttp
        # Ubuntu 22.04: extract development packages locally; sudo is unnecessary.
        (
            cd "$pc_deps/compiler"
            apt-get download gcc-12 g++-12 cpp-12 libgcc-12-dev libstdc++-12-dev libstdc++6 \
                libmbedtls-dev libmbedtls14 libmbedx509-1 libmbedcrypto7 libopusfile-dev libopusfile0 \
                libopus-dev libogg-dev libjpeg-turbo8-dev libjpeg8-dev libsqlite3-dev libmpg123-dev libmpg123-0
            for pc_package in ./*.deb; do dpkg-deb -x "$pc_package" root; done
        )
        ;;
    configure) pc_configure ;;
    rv32-build) pc_rv32_build ;;
    build)
        [[ -f "$pc_build/build.ninja" ]] || pc_configure
        cmake --build "$pc_build" --parallel "${MICROPIXEL_PC_JOBS:-4}"
        ;;
    guest) shift; exec python3 tools/pc.py "$@" ;;
    run)
        shift
        for pc_argument in "$@"; do
            if [[ "$pc_argument" == --home || "$pc_argument" == --music || "$pc_argument" == --music-login || "$pc_argument" == --music-query || "$pc_argument" == --music-song ]]; then
                python3 tools/pc.py guest/apps/netease-music > "$pc_root/build/netease-music-build.log" 2>&1 || { cat "$pc_root/build/netease-music-build.log" >&2; exit 1; }
                break
            fi
        done
        exec "$pc_build/micropixel-pc" "$@"
        ;;
    test) shift; exec bash tools/tests/test_firmware_host.sh --pc "$@" ;;
    *)
        echo "Usage: bash tools/pc.sh {deps|configure|build|rv32-build|guest APP|run [--home|--market|--music] [options]|test}"
        ;;
esac
