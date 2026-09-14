#!/usr/bin/env python3
"""Run live_aim_survey.js on device via Frida and save output."""
import argparse, json, sys, os, frida, time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
JS_PATH = os.path.join(SCRIPT_DIR, 'live_aim_survey.js')
OUTPUT_DIR = os.path.join(SCRIPT_DIR, '..', 'output', 'runtime_metadata')

def main():
    parser = argparse.ArgumentParser(description='Live aim survey')
    parser.add_argument('--pid', type=int, help='Target PID')
    parser.add_argument('--device', default='192.168.5.102:5555', help='ADB device')
    parser.add_argument('--output', default=None, help='Output JSON path')
    args = parser.parse_args()

    if not args.pid:
        # Auto-detect
        import subprocess
        result = subprocess.run(['adb', '-s', args.device, 'shell', 'ps -A | grep crossfire'],
                                capture_output=True, text=True)
        for line in result.stdout.strip().split('\n'):
            parts = line.split()
            if len(parts) >= 2:
                args.pid = int(parts[1])
                break
        if not args.pid:
            print('ERROR: Cannot find game PID', file=sys.stderr)
            sys.exit(1)
        print(f'Auto-detected PID: {args.pid}')

    device = frida.get_device_manager().add_remote_device(args.device)
    session = device.attach(args.pid)
    print(f'Attached to PID {args.pid}')

    with open(JS_PATH, 'r') as f:
        js_code = f.read()

    script = session.create_script(js_code)
    script.load()
    time.sleep(0.5)

    print('Running survey...')
    result = script.exports_sync.survey()

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    if args.output:
        out_path = args.output
    else:
        out_path = os.path.join(OUTPUT_DIR, f'live-survey-{args.pid}.json')

    with open(out_path, 'w') as f:
        json.dump(result, f, indent=2)

    print(f'Saved to {out_path}')
    print(f'PID: {result["pid"]}')
    print(f'Captured at: {result["capturedAt"]}')

    # Summary
    if 'sections' in result:
        s = result['sections']
        if 'listInfo' in s:
            print(f'\nTarget list: {s["listInfo"]["size"]} pawns')
        if 'transformPositions' in s:
            print(f'Transform positions read: {len(s["transformPositions"])}')
            for tp in s['transformPositions']:
                if 'position' in tp:
                    p = tp['position']
                    print(f'  Slot {tp["slot"]}: ({p[0]:.3f}, {p[1]:.3f}, {p[2]:.3f})')
        if 'camera' in s:
            cam = s['camera']
            if 'mainAddress' in cam:
                print(f'\nCamera.main: {cam["mainAddress"]}')
                if 'worldToCameraMatrix' in cam:
                    print(f'  worldToCameraMatrix: {cam["worldToCameraMatrix"][:4]}...')
                if 'projectionMatrix' in cam:
                    print(f'  projectionMatrix: {cam["projectionMatrix"][:4]}...')
                if 'mainFields' in cam:
                    print(f'  Non-zero fields: {list(cam["mainFields"].keys())}')
        if 'screenWidth' in s and 'screenHeight' in s:
            print(f'\nScreen: {s["screenWidth"]}x{s["screenHeight"]}')

        # Pawn field summary
        if 'pawnFieldScan' in s:
            print(f'\n--- Pawn field scan ({len(s["pawnFieldScan"])} pawns) ---')
            for p in s['pawnFieldScan'][:3]:
                print(f'\n  Slot {p["slot"]} [{p["class"]}] @ {p["address"]}:')
                for fname, fdata in p.get('fields', {}).items():
                    if 'error' in fdata:
                        print(f'    {fname}: ERROR {fdata["error"]}')
                    else:
                        print(f'    {fname} (off=0x{fdata["offset"]:x}, kind={fdata["kind"]}) = {fdata["value"]}')

    session.detach()

if __name__ == '__main__':
    main()
