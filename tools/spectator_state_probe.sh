#!/system/bin/sh

# Read-only sampler for the already loaded payload and its cached scene snapshot.
# Usage: spectator_state_probe.sh PID PAYLOAD_BASE_HEX SNAPSHOT_HEX [SECONDS]

game_pid="$1"
# Android's mksh arithmetic is 32-bit on this device. printf/expr keep the
# full 64-bit address, so never use $((...)) for process pointers here.
payload_base=$(printf '%d' "0x$2")
snapshot_addr=$(printf '%d' "0x$3")
duration="${4:-600}"
memory="/proc/$game_pid/mem"

read_hex8() {
    od -An -tx8 -j "$1" -N 8 "$memory" 2>/dev/null | tr -d ' \r\n'
}

read_u4() {
    od -An -tu4 -j "$1" -N 4 "$memory" 2>/dev/null | tr -d ' \r\n'
}

read_u1() {
    od -An -tu1 -j "$1" -N 1 "$memory" 2>/dev/null | tr -d ' \r\n'
}

read_float() {
    od -An -tf4 -j "$1" -N 4 "$memory" 2>/dev/null | tr -d ' \r\n'
}

add_addr() {
    expr "$1" + "$2"
}

hex_to_addr() {
    printf '%d' "0x$1"
}

started=$(date +%s)
last_heartbeat="$started"
last_state=""

while kill -0 "$game_pid" 2>/dev/null; do
    now=$(date +%s)
    [ $((now - started)) -ge "$duration" ] && break

    local_hex=$(read_hex8 "$(add_addr "$snapshot_addr" 6248)")
    [ -z "$local_hex" ] && local_hex="0000000000000000"
    local_addr=$(hex_to_addr "$local_hex")

    sequence=$(read_u4 "$(add_addr "$payload_base" 64656)")
    adjust_active=$(read_u4 "$(add_addr "$payload_base" 64680)")
    gate_bytes=$(od -An -tu1 -j "$(add_addr "$snapshot_addr" 6256)" -N 4 "$memory" 2>/dev/null | tr -s ' ' ',' | tr -d ' \r\n')
    aim_target=$(read_hex8 "$(add_addr "$snapshot_addr" 6264)")

    hp="-"
    controller_hex="0000000000000000"
    controller_back="0000000000000000"
    controller_spectating="-"
    view_target="0000000000000000"
    player_info_hex="0000000000000000"
    player_info_spectating="-"
    respawn_state="-"
    pawn_is_spectated="-"

    if [ "$local_addr" -ne 0 ]; then
        hp=$(read_float "$(add_addr "$local_addr" 112)")
        controller_hex=$(read_hex8 "$(add_addr "$local_addr" 1112)")
        player_info_hex=$(read_hex8 "$(add_addr "$local_addr" 1048)")
        pawn_is_spectated=$(read_u1 "$(add_addr "$local_addr" 1208)")

        controller_addr=$(hex_to_addr "$controller_hex")
        if [ "$controller_addr" -ne 0 ]; then
            controller_back=$(read_hex8 "$(add_addr "$controller_addr" 104)")
            view_target=$(read_hex8 "$(add_addr "$controller_addr" 352)")
            controller_spectating=$(read_u1 "$(add_addr "$controller_addr" 368)")
        fi

        player_info_addr=$(hex_to_addr "$player_info_hex")
        if [ "$player_info_addr" -ne 0 ]; then
            respawn_state=$(read_u4 "$(add_addr "$player_info_addr" 64)")
            player_info_spectating=$(read_u1 "$(add_addr "$player_info_addr" 669)")
        fi
    fi

    state="local=$local_hex hp=$hp ctrl=$controller_hex back=$controller_back ctrlSpec=$controller_spectating view=$view_target info=$player_info_hex infoSpec=$player_info_spectating respawn=$respawn_state pawnSpectated=$pawn_is_spectated gates=$gate_bytes target=$aim_target active=$adjust_active"
    if [ "$state" != "$last_state" ] || [ $((now - last_heartbeat)) -ge 5 ]; then
        printf '%s seq=%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S')" "$sequence" "$state"
        last_state="$state"
        last_heartbeat="$now"
    fi
    sleep 0.25
done
