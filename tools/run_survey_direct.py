#!/usr/bin/env python3
"""Run live_aim_survey via Frida with explicit connection handling."""
import json, sys, os, time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
JS_PATH = os.path.join(SCRIPT_DIR, 'live_aim_survey.js')
OUTPUT_DIR = os.path.join(SCRIPT_DIR, '..', 'output', 'runtime_metadata')

def main():
    import frida

    PID = 3269
    DEVICE_ID = '192.168.5.102:5555'

    with open(JS_PATH, 'r') as f:
        js_code = f.read()

    print(f'Connecting to {DEVICE_ID}...')
    mgr = frida.get_device_manager()
    device = mgr.add_remote_device(DEVICE_ID)
    print(f'Device: {device.name}')

    print(f'Attaching to PID {PID}...')
    try:
        session = device.attach(PID)
    except Exception as e:
        print(f'Attach failed: {e}')
        print('Trying USB...')
        device = frida.get_usb_device(timeout=10)
        session = device.attach(PID)

    print('Session attached, creating script...')
    script = session.create_script(js_code)
    script.load()
    print('Script loaded, running survey...')

    # Call via RPC
    try:
        result = script.exports_sync.survey()
    except Exception as e:
        print(f'RPC error: {e}')
        # Try via on_message
        print('Trying alternative...')
        result_holder = [None]
        done = [False]

        def on_message(msg, data):
            if msg['type'] == 'send':
                result_holder[0] = json.loads(msg['payload'])
                done[0] = True
            elif msg['type'] == 'error':
                print(f'Error: {msg.get("description", msg)}')
                done[0] = True

        script.on('message', on_message)
        script.load()
        script.post(json.dumps({'type': 'call', 'method': 'survey'}))
        timeout = 30
        start = time.time()
        while not done[0] and time.time() - start < timeout:
            time.sleep(0.5)
        result = result_holder[0]
        if result is None:
            print('No result received')
            session.detach()
            sys.exit(1)

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    out_path = os.path.join(OUTPUT_DIR, f'live-survey-{PID}.json')
    with open(out_path, 'w') as f:
        json.dump(result, f, indent=2)

    print(f'\nSaved to {out_path}')

    if 'sections' in result:
        s = result['sections']
        if 'listInfo' in s:
            print(f'\n=== Target list: {s["listInfo"]["size"]} pawns ===')

        if 'transformPositions' in s:
            print(f'\n=== Transform positions ({len(s["transformPositions"])} read) ===')
            for tp in s['transformPositions']:
                if 'position' in tp:
                    p = tp['position']
                    print(f'  Slot {tp["slot"]}: ({p[0]:.3f}, {p[1]:.3f}, {p[2]:.3f})')
                elif 'error' in tp:
                    print(f'  Slot {tp["slot"]}: ERROR {tp["error"]}')

        if 'camera' in s:
            cam = s['camera']
            print(f'\n=== Camera ===')
            if 'mainAddress' in cam:
                print(f'  main: {cam["mainAddress"]}')
            if 'mainError' in cam:
                print(f'  mainError: {cam["mainError"]}')
            if 'worldToCameraMatrix' in cam:
                print(f'  worldToCamera: {cam["worldToCameraMatrix"]}')
            if 'worldToCameraError' in cam:
                print(f'  worldToCameraError: {cam["worldToCameraError"]}')
            if 'projectionMatrix' in cam:
                print(f'  projection: {cam["projectionMatrix"]}')
            if 'projectionError' in cam:
                print(f'  projectionError: {cam["projectionError"]}')
            if 'mainFields' in cam:
                print(f'  Camera fields: {json.dumps(cam["mainFields"], indent=4)}')

        if 'screenWidth' in s and 'screenHeight' in s:
            print(f'\n=== Screen: {s["screenWidth"]}x{s["screenHeight"]} ===')

        if 'pvpPawnFieldNames' in s:
            print(f'\n=== PVPPlayerPawn fields ({len(s["pvpPawnFieldNames"])}) ===')
            for f in s['pvpPawnFieldNames']:
                print(f'  {f["name"]} (off=0x{f["offset"]:x}, kind={f["kind"]})')

        if 'pvpPawnData0' in s:
            print(f'\n=== PVPPlayerPawn slot 0 data ===')
            for fname, fdata in s['pvpPawnData0'].items():
                print(f'  {fname} (off=0x{fdata["offset"]:x}) = {fdata["value"]}')

        if 'pawnFieldScan' in s:
            print(f'\n=== Pawn field scan ({len(s["pawnFieldScan"])} pawns) ===')
            for p in s['pawnFieldScan'][:5]:
                print(f'\n  Slot {p["slot"]} [{p["class"]}] @ {p["address"]}:')
                for fname, fdata in sorted(p.get('fields', {}).items()):
                    if 'error' in fdata:
                        print(f'    {fname}: ERROR {fdata["error"]}')
                    else:
                        print(f'    {fname} (off=0x{fdata["offset"]:x}, kind={fdata["kind"]}) = {fdata["value"]}')

    session.detach()
    print('\nDone.')

if __name__ == '__main__':
    main()
