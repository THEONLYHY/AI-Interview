#!/usr/bin/env bash
#
# virtmic.sh — 一键管理 PulseAudio/PipeWire 虚拟麦克风。
#
# 适用场景:
#   主机没有真实麦克风时,通过 module-pipe-source 模拟一个输入源,
#   并把一段 16 kHz / mono / s16le 的 PCM 文件按实时节奏喂进去,
#   让基于 PortAudio 的程序可以正常采集到音频。
#
# 命令:
#   ./scripts/virtmic.sh start <pcm-file>   启动虚拟麦克风并后台循环喂数据。
#   ./scripts/virtmic.sh stop               停止喂数据并卸载虚拟麦克风。
#   ./scripts/virtmic.sh status             查看当前状态。
#
# 依赖:
#   - pactl       (pulseaudio-utils / pipewire-pulse)
#   - pv          (按字节速率喂数据,避免一次灌完导致服务端误判静音)
#
# 设计:
#   - 加载的 module id 写入 /tmp/virtmic.mod_id;
#   - 后台 feeder 写入 /tmp/virtmic.feeder.pid (整个进程组的 PGID);
#   - 旧的默认输入源写入 /tmp/virtmic.prev_source,stop 时恢复;
#   - feeder 通过 setsid 启动,确保 stop 时可以杀整个进程组,
#     避免 pv 这种子进程被孤立。

set -euo pipefail

PIPE_PATH="/tmp/virtmic"
MOD_ID_FILE="/tmp/virtmic.mod_id"
FEEDER_PID_FILE="/tmp/virtmic.feeder.pid"
PREV_SOURCE_FILE="/tmp/virtmic.prev_source"

SAMPLE_RATE=16000
CHANNELS=1
BYTES_PER_SEC=$((SAMPLE_RATE * 2 * CHANNELS))   # s16le mono => 32000 B/s

log() { echo "[virtmic] $*"; }
err() { echo "[virtmic] error: $*" >&2; }

require_cmd() {
    local cmd="$1"
    local hint="$2"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        err "missing command: $cmd"
        err "install with: $hint"
        exit 1
    fi
}

check_deps() {
    require_cmd pactl "sudo apt install -y pulseaudio-utils"
    require_cmd pv    "sudo apt install -y pv"
}

# 检查 module 是否仍然加载。
module_loaded() {
    [ -f "$MOD_ID_FILE" ] || return 1
    local mod_id
    mod_id=$(cat "$MOD_ID_FILE")
    pactl list short modules | awk '{print $1}' | grep -qx "$mod_id"
}

# 检查 feeder 是否仍然在跑。
feeder_running() {
    [ -f "$FEEDER_PID_FILE" ] || return 1
    local pgid
    pgid=$(cat "$FEEDER_PID_FILE")
    # 进程组组长进程是否还在。
    kill -0 "$pgid" 2>/dev/null
}

cmd_start() {
    local pcm_file="${1:-}"
    if [ -z "$pcm_file" ]; then
        err "usage: $0 start <pcm-file>"
        exit 1
    fi
    if [ ! -f "$pcm_file" ]; then
        err "pcm file not found: $pcm_file"
        exit 1
    fi

    check_deps

    if module_loaded; then
        err "virtmic already running (module id $(cat "$MOD_ID_FILE"))."
        err "run '$0 stop' first if you want to restart."
        exit 1
    fi

    # 1. 记录当前默认输入源,stop 时恢复。
    local prev_source
    prev_source=$(pactl get-default-source 2>/dev/null || true)
    echo "$prev_source" > "$PREV_SOURCE_FILE"

    # 2. 加载 module-pipe-source。
    local mod_id
    mod_id=$(pactl load-module module-pipe-source \
        source_name=virtmic \
        file="$PIPE_PATH" \
        format=s16le \
        rate="$SAMPLE_RATE" \
        channels="$CHANNELS")
    echo "$mod_id" > "$MOD_ID_FILE"
    log "loaded module-pipe-source (id=$mod_id), pipe=$PIPE_PATH"

    # 3. 设为默认输入源。
    pactl set-default-source virtmic
    log "default source set to 'virtmic' (was '${prev_source:-unset}')"

    # 4. 启动 feeder:在新进程组中循环喂数据。
    #    使用 setsid 让 wrapper 进程独立成组,stop 时可以杀整个进程组。
    setsid bash -c "
        trap 'exit 0' TERM INT
        while true; do
            pv -q -L $BYTES_PER_SEC '$pcm_file' > '$PIPE_PATH' || true
        done
    " </dev/null >/dev/null 2>&1 &
    local feeder_pid=$!
    # setsid 启动的子进程的 PID 就是新进程组的 PGID。
    echo "$feeder_pid" > "$FEEDER_PID_FILE"
    log "feeder started (pgid=$feeder_pid, rate=${BYTES_PER_SEC} B/s, loop)"

    log "ready. now run your program; '$0 stop' when done."
}

cmd_stop() {
    local any=0

    # 1. 杀 feeder 进程组。
    if [ -f "$FEEDER_PID_FILE" ]; then
        local pgid
        pgid=$(cat "$FEEDER_PID_FILE")
        if kill -0 "$pgid" 2>/dev/null; then
            kill -TERM -- "-$pgid" 2>/dev/null || true
            # 等最多 1 秒,再强杀。
            for _ in 1 2 3 4 5; do
                kill -0 "$pgid" 2>/dev/null || break
                sleep 0.2
            done
            kill -KILL -- "-$pgid" 2>/dev/null || true
            log "feeder stopped (pgid=$pgid)"
            any=1
        fi
        rm -f "$FEEDER_PID_FILE"
    fi

    # 2. 卸载 module。
    if [ -f "$MOD_ID_FILE" ]; then
        local mod_id
        mod_id=$(cat "$MOD_ID_FILE")
        if pactl list short modules | awk '{print $1}' | grep -qx "$mod_id"; then
            pactl unload-module "$mod_id" 2>/dev/null || true
            log "module $mod_id unloaded"
            any=1
        fi
        rm -f "$MOD_ID_FILE"
    fi

    # 3. 恢复之前的默认输入源。
    if [ -f "$PREV_SOURCE_FILE" ]; then
        local prev
        prev=$(cat "$PREV_SOURCE_FILE")
        if [ -n "$prev" ]; then
            pactl set-default-source "$prev" 2>/dev/null || true
            log "default source restored to '$prev'"
        fi
        rm -f "$PREV_SOURCE_FILE"
    fi

    if [ "$any" -eq 0 ]; then
        log "nothing to stop."
    fi
}

cmd_status() {
    if module_loaded; then
        log "module: loaded (id=$(cat "$MOD_ID_FILE"))"
    else
        log "module: not loaded"
    fi

    if feeder_running; then
        log "feeder: running (pgid=$(cat "$FEEDER_PID_FILE"))"
    else
        log "feeder: not running"
    fi

    local default_src
    default_src=$(pactl get-default-source 2>/dev/null || echo "?")
    log "default source: $default_src"
}

main() {
    local action="${1:-}"
    shift || true
    case "$action" in
        start)  cmd_start "$@" ;;
        stop)   cmd_stop ;;
        status) cmd_status ;;
        *)
            cat <<EOF
usage: $0 <command> [args]

commands:
  start <pcm-file>   load virtual mic and feed PCM in loop (background)
  stop               stop feeder, unload module, restore default source
  status             show current state

example:
  $0 start scripts/test_voice.pcm
  ./build/ai_interview
  $0 stop
EOF
            exit 1
            ;;
    esac
}

main "$@"
