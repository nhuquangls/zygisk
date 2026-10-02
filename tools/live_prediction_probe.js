'use strict';

const PREDICT_RVA = 0x52d4;
const CONTROLLER_RVA = 0x5704;
const PAYLOAD_EXEC_SIZE = 0xc000;
const REPORT_INTERVAL_MS = 250;

function round(value, places) {
    const scale = Math.pow(10, places);
    return Math.round(value * scale) / scale;
}

function leadWeight(speed) {
    if (speed <= 0.25)
        return 0;
    if (speed >= 0.80)
        return 1;
    const position = (speed - 0.25) / (0.80 - 0.25);
    return position * position * (3 - 2 * position);
}

function vec3(address) {
    return {
        x: address.readFloat(),
        y: address.add(4).readFloat(),
        z: address.add(8).readFloat()
    };
}

function project(matrix, point, viewport) {
    const m = [];
    for (let i = 0; i < 16; ++i)
        m.push(matrix.add(i * 4).readFloat());
    const x = m[0] * point.x + m[1] * point.y + m[2] * point.z + m[3];
    const y = m[4] * point.x + m[5] * point.y + m[6] * point.z + m[7];
    const w = m[12] * point.x + m[13] * point.y + m[14] * point.z + m[15];
    if (!Number.isFinite(x) || !Number.isFinite(y) ||
        !Number.isFinite(w) || w <= 0.01)
        return null;
    const nx = x / w;
    const ny = y / w;
    if (Math.abs(nx) > 1 || Math.abs(ny) > 1)
        return null;
    return {
        x: (1 + nx) * viewport.width * 0.5,
        y: (1 - ny) * viewport.height * 0.5
    };
}

const payloadRanges = Process.enumerateRanges('r-x').filter(range =>
    range.size === PAYLOAD_EXEC_SIZE && !range.file);
if (payloadRanges.length !== 1)
    throw new Error('expected one anonymous 0xc000 payload, got ' +
                    payloadRanges.length);

const payloadBase = payloadRanges[0].base;
const predictorAddress = payloadBase.add(PREDICT_RVA);
const controllerAddress = payloadBase.add(CONTROLLER_RVA);
console.log(JSON.stringify({
    event: 'probe_start',
    payload_base: payloadBase.toString(),
    predictor: predictorAddress.toString(),
    controller: controllerAddress.toString()
}));

let latestPrediction = null;
let lastReportMs = 0;
let predictionCalls = 0;
let activeCommands = 0;
let maxSpeed = 0;
let maxLeadPixels = 0;

Interceptor.attach(predictorAddress, {
    onEnter(args) {
        this.predictor = args[0];
        this.target = args[1].toString();
        this.aim = args[3];
        this.predicted = args[5];
    },
    onLeave(result) {
        if (result.toInt32() === 0 || this.predictor.isNull() ||
            this.aim.isNull() || this.predicted.isNull())
            return;
        const aim = vec3(this.aim);
        const predicted = vec3(this.predicted);
        const velocity = vec3(this.predictor.add(28));
        const speed = Math.hypot(velocity.x, velocity.z);
        const lead = {
            x: predicted.x - aim.x,
            y: predicted.y - aim.y,
            z: predicted.z - aim.z
        };
        ++predictionCalls;
        maxSpeed = Math.max(maxSpeed, speed);
        latestPrediction = {
            target: this.target,
            samples: this.predictor.add(40).readU32(),
            aim: aim,
            predicted: predicted,
            velocity: velocity,
            speed: speed,
            horizonMs: 60 * leadWeight(speed),
            leadWorld: Math.hypot(lead.x, lead.z)
        };
    }
});

Interceptor.attach(controllerAddress, {
    onEnter(args) {
        this.snapshot = args[1];
        this.viewport = args[2];
        this.command = args[4];
    },
    onLeave(result) {
        if (result.toInt32() === 0 || !latestPrediction ||
            this.snapshot.isNull() || this.viewport.isNull() ||
            this.command.isNull() || this.command.add(28).readU8() === 0)
            return;
        ++activeCommands;
        const viewport = {
            width: this.viewport.readFloat(),
            height: this.viewport.add(4).readFloat()
        };
        const current = project(this.snapshot, latestPrediction.aim, viewport);
        const predicted = project(this.snapshot, latestPrediction.predicted,
                                  viewport);
        if (!current || !predicted)
            return;
        const leadX = predicted.x - current.x;
        const leadY = predicted.y - current.y;
        const leadPixels = Math.hypot(leadX, leadY);
        maxLeadPixels = Math.max(maxLeadPixels, leadPixels);
        const now = Date.now();
        if (now - lastReportMs < REPORT_INTERVAL_MS)
            return;
        lastReportMs = now;
        console.log(JSON.stringify({
            event: 'prediction',
            target: latestPrediction.target,
            samples: latestPrediction.samples,
            speed_mps: round(latestPrediction.speed, 3),
            horizon_ms: round(latestPrediction.horizonMs, 2),
            lead_world_m: round(latestPrediction.leadWorld, 4),
            lead_px_x: round(leadX, 2),
            lead_px_y: round(leadY, 2),
            lead_px: round(leadPixels, 2),
            predicted_error_px_x: round(this.command.add(8).readFloat(), 2),
            predicted_error_px_y: round(this.command.add(12).readFloat(), 2),
            gyro_x: round(this.command.add(16).readFloat(), 4),
            gyro_y: round(this.command.add(20).readFloat(), 4),
            mode: this.command.add(24).readU32()
        }));
    }
});

setInterval(() => {
    console.log(JSON.stringify({
        event: 'summary',
        prediction_calls: predictionCalls,
        active_commands: activeCommands,
        max_speed_mps: round(maxSpeed, 3),
        max_lead_px: round(maxLeadPixels, 2)
    }));
}, 5000);
