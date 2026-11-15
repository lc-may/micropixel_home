#!/usr/bin/env bash
set -euo pipefail
pc_test_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$pc_test_root"
pc_test_binary="${MICROPIXEL_PC_BINARY:-$pc_test_root/build/pc/micropixel-pc}"
mkdir -p build/pc-test-results
if [[ "${1:-}" == "--run" ]]; then
    shift
    exec timeout -k 2s 30s "$pc_test_binary" "$@"
fi
if [[ "${1:-}" == "--network" ]]; then
    shift
    exec python3 tools/tests/test_pc_network.py "$@"
fi
if [[ "${1:-}" == "--market" ]]; then
    shift
    exec python3 tools/tests/test_pc_market.py "$@"
fi
if [[ "${1:-}" == "--rv32" ]]; then
    shift
    exec python3 tools/tests/test_pc_rv32.py "$@"
fi
if [[ "${1:-}" == "--login" ]]; then
    shift
    python3 tools/pc.py guest/apps/netease-music
    python3 tools/pc.py guest/tests/conformance/auth_contract.cpp
    exec python3 tools/tests/test_pc_login.py "$@"
fi
if [[ "${1:-}" == "--widgets" ]]; then
    shift
    python3 tools/pc.py guest/tests/conformance/music_widgets.cpp
    exec python3 tools/tests/test_pc_widgets.py "$@"
fi
if [[ "${1:-}" == "--launcher" ]]; then
    shift
    python3 tools/pc.py guest/apps/netease-music
    python3 tools/pc.py guest/tests/conformance/timer_counter.cpp
    exec python3 tools/tests/test_pc_launcher.py "$@"
fi
if [[ "${1:-}" == "--feed" ]]; then
    shift
    python3 tools/pc.py guest/apps/netease-music
    exec python3 tools/tests/test_pc_feed.py "$@"
fi
if [[ "${1:-}" == "--home" ]]; then
    shift
    python3 tools/pc.py guest/apps/netease-music
    exec python3 tools/tests/test_pc_home.py "$@"
fi
if [[ "${1:-}" == "--music" ]]; then
    shift
    python3 tools/pc.py guest/apps/netease-music
    python3 tools/pc.py guest/tests/conformance/network_media.cpp --extra-source guest/apps/netease-music/weapi.cpp
    exec python3 tools/tests/test_pc_music.py "$@"
fi
for pc_test_app in sdk_hello audio_synth audio_pcm_stream timer_counter graphics_protocol direct_surface_present; do
    python3 tools/pc.py "guest/tests/conformance/$pc_test_app.cpp" > "build/pc-test-results/$pc_test_app.build.log" 2>&1
    timeout -k 2s 20s "$pc_test_binary" --headless --autostart --exit-on-app-complete --seconds 8 \
        "build/pc-apps/$pc_test_app/$pc_test_app.pc.json" > "build/pc-test-results/$pc_test_app.log" 2>&1
    rg -q 'Guest completed: result=0' "build/pc-test-results/$pc_test_app.log"
    echo "PASS PC Guest: $pc_test_app"
done
python3 tools/tests/test_pc_render.py
python3 tools/tests/test_pc_network.py
python3 tools/tests/test_pc_market.py
python3 tools/pc.py guest/apps/netease-music
python3 tools/pc.py guest/tests/conformance/network_media.cpp --extra-source guest/apps/netease-music/weapi.cpp
python3 tools/tests/test_pc_music.py

python3 tools/pc.py guest/tests/conformance/auth_contract.cpp
python3 tools/tests/test_pc_login.py
