'use strict';

// Diagnostic RVAs for the locally built 2.8.4 payload. The packaged image is
// stripped (SHA-256 a8a4bf1e...); its executable bytes match the symbol build.
// The probe only reads state and observes the payload's own calls.
const EXEC_SIZE = 0xc000;
const SCENE_POLL_RVA = 0x7be8;
const CONTROLLER_RVA = 0x5704;
const SET_ADJUSTMENT_RVA = 0xac78;
const SNAP_LOCAL = 6248;
const SNAP_FLAGS = 6256;
const SNAP_TARGET = 6264;
const LAYOUT_OFFSET = 15 * Process.pointerSize;
const HEALTH_FIELD = 1;
const DESTROYED_FIELD = 6;

const ranges = Process.enumerateRanges('r-x').filter(range =>
    range.size === EXEC_SIZE && !range.file);
if (ranges.length !== 1)
    throw new Error('Expected one anonymous 0xc000 payload mapping; found ' +
                    ranges.length);
const base = ranges[0].base;

let scene = ptr('0');
let latest = null;
let previousKey = '';
let lastSampleMs = 0;
let pollFailures = 0;
let controllerCalls = 0;
let activeCommands = 0;
let publishedActive = false;

function log(data) {
    data.time = new Date().toISOString();
    console.log(JSON.stringify(data));
}

function localState(snapshot) {
    const pawn = snapshot.add(SNAP_LOCAL).readPointer();
    const state = {
        local: pawn.toString(),
        health: null,
        destroyed: null,
        sniper: snapshot.add(SNAP_FLAGS).readU8() !== 0,
        scope: snapshot.add(SNAP_FLAGS + 1).readU8() !== 0,
        sniperAimEnabled: snapshot.add(SNAP_FLAGS + 2).readU8() !== 0,
        doingAim: snapshot.add(SNAP_FLAGS + 3).readU8() !== 0,
        target: snapshot.add(SNAP_TARGET).readPointer().toString()
    };
    if (pawn.isNull() || scene.isNull())
        return state;
    try {
        const healthOffset = scene.add(LAYOUT_OFFSET + HEALTH_FIELD * 8)
                                  .readU32();
        const destroyedOffset = scene.add(LAYOUT_OFFSET + DESTROYED_FIELD * 8)
                                     .readU32();
        if (healthOffset === 0 || healthOffset > 0x4000 ||
            destroyedOffset === 0 || destroyedOffset > 0x4000)
            return state;
        const health = pawn.add(healthOffset).readFloat();
        state.health = Number.isFinite(health) ?
            Math.round(health * 100) / 100 : null;
        state.destroyed = pawn.add(destroyedOffset).readU8() !== 0;
    } catch (_) {
        // A pawn can disappear during a scene transition. Record it as unreadable.
    }
    return state;
}

function emitSample(force) {
    if (!latest)
        return;
    const now = Date.now();
    const state = latest;
    const life = state.health === null ? 'unknown' :
        (state.destroyed || state.health <= 0 ? 'dead' : 'alive');
    const key = [state.local, life, state.sniper, state.scope,
                 state.sniperAimEnabled, state.doingAim, state.target,
                 state.mode, state.commandActive, publishedActive].join('|');
    if (!force && key === previousKey && now - lastSampleMs < 500)
        return;
    previousKey = key;
    lastSampleMs = now;
    log({event: 'state', ...state, life: life,
         publishedActive: publishedActive});
}

log({event: 'probe_ready', payloadBase: base.toString(),
     scenePoll: base.add(SCENE_POLL_RVA).toString(),
     controller: base.add(CONTROLLER_RVA).toString(),
     setAdjustment: base.add(SET_ADJUSTMENT_RVA).toString()});

Interceptor.attach(base.add(SCENE_POLL_RVA), {
    onEnter(args) {
        scene = args[0];
    },
    onLeave(result) {
        if (result.toInt32() !== 0)
            return;
        ++pollFailures;
        if (pollFailures === 1 || pollFailures % 25 === 0)
            log({event: 'scene_poll_failed', pollFailures: pollFailures,
                 lastState: latest});
    }
});

Interceptor.attach(base.add(CONTROLLER_RVA), {
    onEnter(args) {
        this.snapshot = args[1];
        this.command = args[4];
    },
    onLeave(result) {
        ++controllerCalls;
        try {
            const state = localState(this.snapshot);
            state.mode = this.command.add(24).readU32();
            state.commandActive = this.command.add(28).readU8() !== 0;
            state.gyroX = Math.round(this.command.add(16).readFloat() * 10000) / 10000;
            state.gyroY = Math.round(this.command.add(20).readFloat() * 10000) / 10000;
            state.controllerReturned = result.toInt32() !== 0;
            if (state.commandActive)
                ++activeCommands;
            latest = state;
            emitSample(false);
        } catch (error) {
            log({event: 'sample_read_failed', message: String(error)});
        }
    }
});

Interceptor.attach(base.add(SET_ADJUSTMENT_RVA), {
    onEnter(args) {
        const next = args[0].toInt32() !== 0;
        if (next !== publishedActive) {
            publishedActive = next;
            emitSample(true);
        }
    }
});

setInterval(() => {
    log({event: 'summary', controllerCalls: controllerCalls,
         activeCommands: activeCommands, pollFailures: pollFailures,
         lastState: latest, publishedActive: publishedActive});
}, 5000);
