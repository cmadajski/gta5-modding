#!/usr/bin/env python3
"""Install this build with a journal and copies of every replaced file."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--game-dir', type=Path, required=True)
    parser.add_argument('--reshade-dll', type=Path, required=True)
    parser.add_argument('--backup-root', type=Path, required=True)
    parser.add_argument('--settings-file', type=Path)
    parser.add_argument(
        '--effect', choices=('vectors', 'blur'), default='vectors',
        help='effect enabled in the preset (default: vectors)',
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    game = args.game_dir.resolve()
    if not (game / 'GTA5_Enhanced.exe').is_file():
        parser.error('Target is not a GTA V Enhanced executable directory')
    addon = root / 'build-windows/NativeMotionVectors.addon64'
    vectors_shader = root / 'shaders/NativeMotionVectors.fx'
    blur_shader = root / 'shaders/NativeMotionBlur.fx'
    blur_preset = root / 'presets/NativeMotionBlur.ini' if args.effect == 'blur' else None
    shaders = [vectors_shader]
    if blur_preset is not None or blur_shader.is_file():
        shaders.append(blur_shader)
    sources = [args.reshade_dll, addon, *shaders]
    if blur_preset is not None:
        sources.append(blur_preset)
    for source in sources:
        if not source.is_file():
            parser.error(f'Missing input: {source}')
    reshade = '''[GENERAL]
EffectSearchPaths=.\\reshade-shaders\\Shaders
TextureSearchPaths=.\\reshade-shaders\\Textures
PresetPath=.\\NativeMotionPreset.ini
StartupPresetPath=.\\NativeMotionPreset.ini
SkipLoadingDisabledEffects=0

[INPUT]
KeyOverlay=36,0,0,0
KeyEffects=118,0,0,0
KeyScreenshot=119,0,0,0

[OVERLAY]
TutorialProgress=4

[SCREENSHOT]
FileFormat=1
SavePath=.\\NativeMotionCaptures
SaveBeforeShot=1
SaveOverlayShot=1
'''
    preset = b'''Techniques=NativeMotionVisualization@NativeMotionVectors.fx
TechniqueSorting=NativeMotionVisualization@NativeMotionVectors.fx

[NativeMotionVectors.fx]
NativeMotionDisplay=0
NativeMotionUnits=0
NativeMotionGain=1.000000
NativeMotionOpacity=0.650000
NativeMotionShowStatus=1
'''
    if blur_preset is not None:
        preset = blur_preset.read_bytes()
    targets = {
        'dxgi.dll': args.reshade_dll.read_bytes(),
        'NativeMotionVectors.addon64': addon.read_bytes(),
        **{f'reshade-shaders/Shaders/{shader.name}': shader.read_bytes() for shader in shaders},
        'ReShade.ini': reshade.encode(),
        'NativeMotionPreset.ini': preset,
    }
    backup = args.backup_root.resolve() / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    backup.mkdir(parents=True, exist_ok=False)
    journal = {'game_directory': str(game), 'created_utc': datetime.now(timezone.utc).isoformat(), 'files': []}
    for relative, content in targets.items():
        target = game / relative
        entry = {'path': str(target), 'existed': target.exists(), 'installed_sha256': hashlib.sha256(content).hexdigest()}
        if target.exists():
            saved = backup / relative
            saved.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(target, saved)
            entry.update(backup=str(saved), original_sha256=digest(saved))
        journal['files'].append(entry)
    if args.settings_file and args.settings_file.is_file():
        saved = backup / 'original-game-settings.xml'
        shutil.copy2(args.settings_file, saved)
        journal['settings_backup'] = {'path': str(args.settings_file), 'backup': str(saved), 'sha256': digest(saved)}
    journal_path = backup / 'install-journal.json'
    journal_path.write_text(json.dumps(journal, indent=2) + '\n')
    for relative, content in targets.items():
        target = game / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_name(target.name + '.native-motion-install')
        temporary.write_bytes(content)
        temporary.replace(target)
        assert digest(target) == hashlib.sha256(content).hexdigest()
    (game / 'NativeMotionCaptures').mkdir(exist_ok=True)
    print(f'Installed {len(targets)} verified files. Journal: {journal_path}')


if __name__ == '__main__':
    main()
