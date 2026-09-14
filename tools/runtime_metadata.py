"""Query live IL2CPP metadata through an explicitly configured Frida connection.

No hooks or game method calls. Requires the process to be initialized.
"""
import argparse
import json
from pathlib import Path
import frida

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CLASSES = [
    {'namespace': 'WNEngine', 'name': name} for name in [
        'AttackableTargetsManager', 'AttackableTarget', 'Pawn', 'PlayerController',
        'Actor', 'GameWorld', 'World', 'GameManager', 'GameEngine',
        'PlayerPawn', 'GamePlayer', 'LocalPlayer', 'CameraController', 'PlayerInfo',
    ]
] + [{'image': 'UnityEngine.dll', 'namespace': 'UnityEngine', 'name': name}
     for name in ['Camera', 'Transform', 'Component', 'Object']]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', required=True, type=int)
    parser.add_argument('--remote', default='127.0.0.1:27043')
    parser.add_argument('--requests', type=Path)
    parser.add_argument('--inspect-roots', action='store_true')
    parser.add_argument('--output', type=Path, default=ROOT / 'output/runtime_metadata/metadata.json')
    args = parser.parse_args()
    requests = json.loads(args.requests.read_text(encoding='utf-8-sig')) if args.requests else DEFAULT_CLASSES
    device = frida.get_device_manager().add_remote_device(args.remote)
    session = device.attach(args.pid)
    try:
        script = session.create_script((ROOT / 'tools/runtime_metadata.js').read_text(encoding='utf-8'))
        script.load()
        data = script.exports_sync.collect(requests, args.inspect_roots)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding='utf-8')
        print('PID:', data['pid'], 'base:', data['base'], 'assemblies:', len(data['assemblies']))
        for klass in data['classes']:
            print(f"{klass['namespace']}.{klass['name']}: {len(klass['fields'])} fields, {len(klass['methods'])} methods, size={klass['size']}")
        print('Missing:', ', '.join(r['name'] for r in data['missing']))
        for root in data.get('roots', []):
            print('Root:', root['address'], 'list:', root.get('list'),
                  'capacity:', root.get('capacity'), 'stable:', root.get('stable'),
                  'samples:', len(root.get('samples', [])))
        print('Saved:', args.output)
    finally:
        session.detach()


if __name__ == '__main__':
    main()
