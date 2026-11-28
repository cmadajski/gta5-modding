#!/usr/bin/env python3
"""Request and archive one four-frame native-vector burst from a running game."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=45)
    args = parser.parse_args()
    if not (args.game_dir / 'GTA5_Enhanced.exe').is_file():
        parser.error('Target is not a GTA V Enhanced executable directory')
    if not 1 <= args.timeout <= 300:
        parser.error('--timeout must be between 1 and 300 seconds')
    captures = args.game_dir / 'NativeMotionCaptures'
    captures.mkdir(exist_ok=True)
    existing = {p.name for p in captures.glob('NativeWheel-*.json')}
    args.output.mkdir(parents=True, exist_ok=False)
    request = captures / 'CaptureWheel.request'
    with request.open('x') as stream:
        stream.write(datetime.now(timezone.utc).isoformat() + '\n')
    print('Requested four raw motion buffers and matching scenes. Keep the wheel visible and moving.', flush=True)
    start = time.monotonic()
    copied = []
    burst = None
    while time.monotonic() - start < args.timeout:
        for path in sorted(captures.glob('NativeWheel-*.json')):
            if path.name in existing or path.name in copied:
                continue
            prefix = path.stem.rsplit('-', 1)[0]
            if burst is not None and burst != prefix:
                continue
            data = json.loads(path.read_text())
            payloads = [path.with_name(path.stem + suffix) for suffix in
                        ('-SceneBefore.bin', '-SceneAfter.bin', '-Motion.bin')]
            if not all(p.is_file() for p in payloads):
                continue
            if data.get('wheel_mask_captured', False):
                payloads.extend(path.with_name(path.stem + suffix) for suffix in
                    ('-WheelMask.bin', '-WheelDepth.bin', '-SceneDepth.bin', '-WheelParameters.bin'))
                if not all(p.is_file() for p in payloads):
                    continue
            burst = prefix
            for payload in payloads:
                shutil.copy2(payload, args.output / payload.name)
            shutil.copy2(path, args.output / path.name)
            copied.append(path.name)
            print(f'Archived {len(copied)}/4: sequence={data["sequence"]}, submissions={data["submissions"]}', flush=True)
        if len(copied) == 4:
            break
        time.sleep(0.25)
    log = args.game_dir / 'ReShade.log'
    if log.is_file():
        shutil.copy2(log, args.output / log.name)
    (args.output / 'capture-request.json').write_text(json.dumps({
        'completed': len(copied) == 4, 'captures': copied,
        'game_directory': str(args.game_dir.resolve()), 'elapsed_seconds': time.monotonic() - start,
    }, indent=2) + '\n')
    if len(copied) != 4:
        parser.exit(1, f'Only {len(copied)}/4 items arrived. Inspect ReShade.log; do not treat this as a complete burst.\n')
    print(f'Raw capture archived at {args.output.resolve()}.')


if __name__ == '__main__':
    main()
