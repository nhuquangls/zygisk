"""Development reader: invoke the independent root ELF, never attach to the game.

The helper permits only the configured game process and performs bounded reads.
Every response is checked against the first observed process identity.
"""
import pathlib
import re
import shlex
import subprocess
import struct


class RemoteProbe:
    def __init__(self, serial='192.168.5.102:5555'):
        adb = next(pathlib.Path('C:/Users/Admin/AppData/Local/Microsoft/WinGet/Packages')
                   .glob('Google.PlatformTools*/platform-tools/adb.exe'))
        self.base_command = [str(adb), '-s', serial]
        self.identity = None
        self.base = None

    def read(self, address, size, rva=False):
        command = '/data/local/tmp/cf_remote_probe {} {} {}'.format(
            '--rva' if rva else '--address', hex(address), size)
        result = subprocess.run(self.base_command + ['shell', 'su -c ' + shlex.quote(command)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=15)
        if result.returncode:
            raise RuntimeError(result.stderr + result.stdout)
        match = re.search(r'pid=(\d+) bias=(0x[0-9a-f]+) build_id=([0-9a-f]+)', result.stderr)
        if not match:
            raise RuntimeError('Missing target identity')
        identity = match.groups()
        if self.identity is not None and self.identity != identity:
            raise RuntimeError('Game process changed during capture')
        self.identity = identity
        self.base = int(identity[1], 16)
        data = bytes.fromhex(result.stdout.strip())
        if len(data) != size:
            raise RuntimeError('Incomplete external read')
        return data

    def pointer(self, address, rva=False):
        return struct.unpack('<Q', self.read(address, 8, rva))[0]

    def string(self, address):
        data = self.read(address, min(128, 4096-(address & 4095)))
        return data.split(b'\0', 1)[0].decode('utf-8', errors='replace')
