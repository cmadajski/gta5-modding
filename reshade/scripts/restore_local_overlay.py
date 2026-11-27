#!/usr/bin/env python3
"""Restore a local Native Motion Vectors installation from its journal."""

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat


ARTIFACTS = {
    Path('dxgi.dll'),
    Path('NativeMotionVectors.addon64'),
    Path('reshade-shaders/Shaders/NativeMotionBlur.fx'),
    Path('reshade-shaders/Shaders/NativeMotionVectors.fx'),
    Path('ReShade.ini'),
    Path('NativeMotionPreset.ini'),
}
SETTINGS_SUFFIX = ('Rockstar Games', 'GTAV Enhanced', 'settings.xml')


@dataclass(frozen=True)
class Backup:
    content: bytes
    mode: int
    digest: str


@dataclass(frozen=True)
class Change:
    target: Path
    current_digest: str | None
    backup: Backup | None


def absolute_path(value):
    if not isinstance(value, str):
        raise ValueError('Journal paths must be strings.')
    path = Path(value)
    if path.anchor != '/' or '..' in path.parts:
        raise ValueError(f'Expected an absolute path without traversal: {value}')
    return path


def recorded_digest(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
        raise ValueError('Invalid SHA-256 in journal.')
    return value


@contextmanager
def open_directory(path):
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    directory = os.open(path.anchor, flags)
    try:
        for part in path.parts[1:]:
            child = os.open(part, flags, dir_fd=directory)
            os.close(directory)
            directory = child
        yield directory
    finally:
        os.close(directory)


@contextmanager
def open_regular(directory, name):
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
    descriptor = os.open(name, flags, dir_fd=directory)
    with os.fdopen(descriptor, 'rb') as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise ValueError(f'Expected a regular file: {name}')
        yield stream


def current_digest(directory, name):
    try:
        with open_regular(directory, name) as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest()
    except FileNotFoundError:
        return None


def inspect_target(target, *, allow_missing_parent=False):
    try:
        with open_directory(target.parent) as directory:
            return current_digest(directory, target.name)
    except FileNotFoundError:
        if not allow_missing_parent:
            raise
        return None


def load_backup(entry, expected_path, digest_key):
    path = absolute_path(entry['backup'])
    if path != expected_path:
        raise ValueError(f'Backup is outside its expected journal location: {path}')
    expected = recorded_digest(entry[digest_key])
    with open_directory(path.parent) as directory, open_regular(directory, path.name) as stream:
        content = stream.read()
        mode = os.fstat(stream.fileno()).st_mode & 0o777
    if hashlib.sha256(content).hexdigest() != expected:
        raise ValueError(f'Backup SHA-256 mismatch: {path}')
    return Backup(content, mode, expected)


def plan_changes(journal, journal_dir, force, restore_settings):
    game = absolute_path(journal['game_directory'])
    with open_directory(game) as directory, open_regular(directory, 'GTA5_Enhanced.exe'):
        pass

    entries = journal['files']
    if not isinstance(entries, list):
        raise ValueError('Expected a list of game artifacts in the journal.')
    changes = []
    seen = set()
    for entry in entries:
        target = absolute_path(entry['path'])
        if not target.is_relative_to(game) or target.relative_to(game) not in ARTIFACTS:
            raise ValueError(f'Unexpected game artifact path: {target}')
        if target in seen:
            raise ValueError(f'Duplicate game artifact path: {target}')
        seen.add(target)
        installed = recorded_digest(entry['installed_sha256'])
        existed = entry['existed']
        if not isinstance(existed, bool):
            raise ValueError(f'Invalid original-file record: {target}')
        backup = None
        if existed:
            backup = load_backup(entry, journal_dir / target.relative_to(game), 'original_sha256')

        current = inspect_target(target, allow_missing_parent=not existed)
        original = backup.digest if backup else None
        if current == original:
            continue
        if current != installed and not force:
            raise ValueError(f'Installed file has changed or is missing: {target}. Review it before using --force.')
        changes.append(Change(target, current, backup))

    if restore_settings:
        if 'settings_backup' not in journal:
            raise ValueError('The journal has no game settings backup.')
        entry = journal['settings_backup']
        target = absolute_path(entry['path'])
        if target.parts[-len(SETTINGS_SUFFIX):] != SETTINGS_SUFFIX:
            raise ValueError(f'Unexpected GTA V Enhanced settings path: {target}')
        backup = load_backup(entry, journal_dir / 'original-game-settings.xml', 'sha256')
        current = inspect_target(target)
        if current != backup.digest:
            changes.append(Change(target, current, backup))

    return changes


def verify_target(directory, change):
    if current_digest(directory, change.target.name) != change.current_digest:
        raise ValueError(f'Target changed after validation: {change.target}. Run the preview again.')


def apply_change(change):
    with open_directory(change.target.parent) as directory:
        if change.backup is None:
            verify_target(directory, change)
            os.unlink(change.target.name, dir_fd=directory)
            return

        temporary = f'.native-motion-restore-{secrets.token_hex(12)}'
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600, dir_fd=directory)
        try:
            with os.fdopen(descriptor, 'wb') as stream:
                stream.write(change.backup.content)
                os.fchmod(stream.fileno(), change.backup.mode)
            verify_target(directory, change)
            os.replace(temporary, change.target.name, src_dir_fd=directory, dst_dir_fd=directory)
        finally:
            try:
                os.unlink(temporary, dir_fd=directory)
            except FileNotFoundError:
                pass


def main():
    parser = argparse.ArgumentParser(
        description='Preview how to undo a local overlay installation. Close GTA V before applying changes.',
        epilog='Keep the original backups next to the journal. Captures and directories are preserved.',
    )
    parser.add_argument('journal', type=Path, help='path to install-journal.json')
    parser.add_argument('--apply', action='store_true', help='apply the changes shown by the preview')
    parser.add_argument('--force', action='store_true', help='permit overwriting or removing changed installed files')
    parser.add_argument(
        '--restore-settings', action='store_true', help='also replace GTA V Enhanced settings.xml with its verified backup',
    )
    args = parser.parse_args()

    try:
        journal_path = absolute_path(str(args.journal.absolute()))
        with open_directory(journal_path.parent) as directory, open_regular(directory, journal_path.name) as stream:
            journal = json.load(stream)
        changes = plan_changes(journal, journal_path.parent, args.force, args.restore_settings)
        for change in changes:
            if args.apply:
                apply_change(change)
                action = 'Restored' if change.backup else 'Removed'
            else:
                action = 'Would restore' if change.backup else 'Would remove'
            print(f'{action}: {change.target}')
        if not changes:
            print('No changes needed.')
    except (KeyError, OSError, TypeError, ValueError) as error:
        parser.exit(1, f'Restore stopped: {error}\n')


if __name__ == '__main__':
    main()
