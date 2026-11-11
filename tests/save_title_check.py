"""Check profile-picker UI decisions and queued worker completion without game files."""
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    work = ROOT / 'build' / 'test' / 'save_title'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('save_title_check.exe' if os.name == 'nt' else 'save_title_check')
    cc = os.environ.get('CC', 'clang')
    subprocess.check_call([cc, '-std=gnu11', '-O1', '-g', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'save_title_check.c'), str(ROOT / 'runtime' / 'save_title.c'),
                           '-o', str(exe)])
    failed = 0
    for case in ('save', 'cancel', 'empty', 'queued', 'queued-empty', 'failure', 'context'):
        failed += subprocess.run([str(exe), case]).returncode != 0
    print('save_title_check:', 'FAILED' if failed else 'ok')
    return bool(failed)


if __name__ == '__main__':
    raise SystemExit(main())
