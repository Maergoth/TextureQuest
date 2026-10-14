"""Direct ready-file copies with streamed SHA and inexpensive recorded status.

No all-resource decompression or repeated destination/stage readback. Receipt
metadata is a practical unchanged-file check, not a hostile-metadata guarantee.
Explicit Verify still uses the legacy full-byte verification API.
"""
from contextlib import ExitStack
import hashlib
import ctypes
from ctypes import wintypes
import msvcrt
import os
from pathlib import Path
import re
import shutil
import stat
import uuid

from texture_swapper import core
from texturequest_custody import FileCustody, DirectoryCustody, KERNEL, _information
import texturequest_release as release
from texturequest_progress import OverallProgress


def stamp(path):
    info = core._plain(path).stat()
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise ValueError('Archive is not an ordinary single file')
    return dict(device=info.st_dev, file_id=info.st_ino, bytes=info.st_size,
                modified_ns=info.st_mtime_ns)


def stream_stamp(stream):
    info = os.fstat(stream.fileno())
    return dict(device=info.st_dev, file_id=info.st_ino, bytes=info.st_size,
                modified_ns=info.st_mtime_ns)


class FastFileCustody(FileCustody):
    """Official direct files need their complete SHA, not payload inflation."""
    def remember_verified(self, row, digest):
        if digest != row['sha256'] or os.fstat(self.stream.fileno()).st_size != row['bytes']:
            raise ValueError('Archive checksum differs: ' + row['name'])
        self._verified = (row['bytes'], row['sha256'], stream_stamp(self.stream))

    def matches(self, row, pause=None):
        release.check_pause(pause)
        self.identity()
        key = (row['bytes'], row['sha256'], stream_stamp(self.stream))
        if getattr(self, '_verified', None) == key:
            return True
        if not super().matches(row, pause):
            return False
        self._verified = key
        return True

    def checked(self, row, pause=None):
        if not self.matches(row, pause):
            raise ValueError('Archive checksum differs: ' + row['name'])
        self.identity()


class DownloadCustody(FastFileCustody):
    """Keep the verified download writer held through its native cache commit."""
    def __enter__(self):
        handle = KERNEL.CreateFileW(str(self.path), 0xC0000000 | 0x10000, 1, None, 4, 0x00200000 | 0x80, None)
        if not handle or handle == wintypes.HANDLE(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            info = _information(handle)
            if info.attributes & (0x10 | 0x400) or info.links != 1:
                raise ValueError('Download partial is linked or not a regular file')
            fd = msvcrt.open_osfhandle(int(handle), os.O_RDWR | os.O_BINARY)
        except BaseException:
            KERNEL.CloseHandle(handle)
            raise
        self.handle = handle
        try:
            self.stream = os.fdopen(fd, 'r+b')
        except BaseException:
            os.close(fd)
            self.handle = None
            raise
        self.initial_id = self.identity()
        return self


def unchanged(path, record, row):
    if record.get('status') != 'installed' or record.get('sha256') != row['sha256'] or record.get('bytes') != row['bytes']:
        return False
    try:
        return stamp(path) == record.get('installed_file')
    except (OSError, ValueError):
        return False


def copy_archive(source, output, row, progress=None):
    """One source read while copying; verifies incoming bytes without rereading."""
    source.seek(0)
    digest = hashlib.sha256()
    total = 0
    while chunk := source.read(core.CHUNK):
        total += len(chunk)
        if total > row['bytes']:
            raise ValueError('Prepared archive length changed')
        output.write(chunk)
        digest.update(chunk)
        if progress is not None:
            progress(total)
    output.flush()
    os.fsync(output.fileno())
    if total != row['bytes'] or digest.hexdigest() != row['sha256']:
        raise ValueError('Prepared archive checksum changed; download it again')
    return digest.hexdigest()


def _staged_path(paks, record):
    name = record.get('staged_name')
    pattern = re.escape('.' + record['name'] + '.swapper-') + r'[0-9a-f]{32}\.tmp'
    if not isinstance(name, str) or not re.fullmatch(pattern, name):
        raise ValueError('Invalid staged file in the recovery manifest')
    return core._plain(paks / name, paks)


def _same_id(current, saved):
    return isinstance(saved, dict) and all(current.get(k) == saved.get(k) for k in ('device', 'file_id'))


def _recover_partial(paks, record):
    path = _staged_path(paks, record)
    if not path.exists():
        return
    with FileCustody(path) as held:
        if not _same_id(stream_stamp(held.stream), record.get('staged_file')):
            raise ValueError('Unknown replaced staging file is preserved')
        held.remove()


def _commit(staged, target, directories, expected):
    # Held stage cannot be changed/deleted during its exact native rename.
    with FileCustody(staged) as held:
        if stream_stamp(held.stream) != expected:
            raise ValueError('Staged copy metadata changed; preserved')
        held.rename_replace(target, directories)


def install(swapper, prepared, game_dir, progress=None, overall_progress=None):
    notify = progress or (lambda _: None)
    with core._lock(swapper.state_dir / 'operation.lock'), core._game_lock(core.validate_game(game_dir)):
        ready = swapper.preflight(prepared, game_dir)
        if ready['missing']:
            raise ValueError('These archives are not installed in the selected game: ' + ', '.join(ready['missing']))
        overall = OverallProgress(ready['archives'], overall_progress)
        paks, path = swapper._game_manifest(game_dir)
        core.ensure_game_closed()
        value = swapper._manifest(paks, path)
        managed = {row['name'].casefold(): row for row in value['archives']}
        attempt = uuid.uuid4().hex
        value.update(status='installing', operation='install', attempt_id=attempt,
                     pack_id=prepared['id'], error=None, message=None, install_method='direct-copy-v1')
        core._save(path, value)
        try:
            with DirectoryCustody(paks) as directories:
                for row in ready['archives']:
                    target = core._plain(row['target'], paks)
                    overall.start(row, 'Installing ' + target.name)
                    old = managed.get(target.name.casefold(), {})
                    if unchanged(target, old, row):
                        if core._readonly(target):
                            core.ensure_game_closed()
                            core._set_readonly(target, False)
                        notify('Already installed: ' + target.name)
                        overall.complete('Already installed: ' + target.name)
                        continue
                    # Lost acknowledgment after rename: same exact copied file
                    # now exists at target, so finish its receipt without reread.
                    if (old.get('status') == 'staged' and old.get('sha256') == row['sha256']
                            and old.get('bytes') == row['bytes'] and stamp(target) == old.get('staged_file')):
                        old.update(status='installed', installed_at=core._now(), installed_file=stamp(target), readonly=False)
                        core._save(path, value)
                        overall.complete('Already installed: ' + target.name)
                        continue
                    if old.get('staged_name'):
                        _recover_partial(paks, old)
                    source_path = core._plain(row['path'])
                    if source_path.stat().st_size != row['bytes']:
                        raise ValueError('Prepared archive length changed')
                    staged_name = f'.{target.name}.swapper-{attempt}.tmp'
                    record = dict(name=target.name, sha256=row['sha256'], bytes=row['bytes'], managed=True,
                                  status='pending', previous_sha256=None, previous_readonly=core._readonly(target),
                                  original_sha256=old.get('original_sha256'), selected_at=core._now(),
                                  staged_name=staged_name, staged_file=None)
                    managed[target.name.casefold()] = record
                    value['archives'] = list(managed.values())
                    core._save(path, value)
                    staged = core._plain(paks / staged_name, paks)
                    if shutil.disk_usage(paks).free < row['bytes'] + 16 * core.CHUNK:
                        raise OSError('Not enough space in the game folder to stage this archive')
                    notify('Installing ' + target.name)
                    with FileCustody(source_path) as source, staged.open('xb') as output:
                        before = stream_stamp(source.stream)
                        if before['bytes'] != row['bytes']:
                            raise ValueError('Prepared archive length changed')
                        record['staged_file'] = stream_stamp(output)
                        core._save(path, value)
                        if overall_progress is None:
                            copied = copy_archive(source.stream, output, row)
                        else:
                            copied = copy_archive(source.stream, output, row,
                                                  lambda done: overall.update(done, 'Installing ' + target.name))
                        if stream_stamp(source.stream) != before:
                            raise ValueError('Prepared archive changed during copy')
                        record.update(status='staged', copied_sha256=copied)
                    record['staged_file'] = stamp(staged)
                    core._save(path, value)
                    core.ensure_game_closed()
                    if core._readonly(target):
                        core._set_readonly(target, False)
                    _commit(staged, target, directories, record['staged_file'])
                    record.update(status='installed', installed_at=core._now(), installed_file=stamp(target), readonly=False)
                    core._save(path, value)
                    overall.complete('Installed ' + target.name)
            value.update(status='installed', completed_at=core._now())
            core._save(path, value)
            core._save(path.parent / 'attempts' / (attempt + '.json'), dict(value))
        except BaseException as error:
            value.update(status='interrupted', error=str(error))
            core._save(path, value)
            core._save(path.parent / 'attempts' / (attempt + '.json'), dict(value))
            raise
        notify('Installed. Rerun TextureQuest after a game patch.')
        return status(swapper, game_dir)


def status(swapper, game_dir):
    paks, path = swapper._game_manifest(game_dir)
    value = swapper._manifest(paks, path)
    rows = []
    for old in value['archives']:
        row = dict(old, exists=None, readonly=None, actual_sha256=None, actual_bytes=None,
                   integrity='unverified', verification_error=None, verification='recorded-file-metadata')
        try:
            target = core._plain(paks / old['name'], paks)
            if not target.exists():
                row.update(exists=False, readonly=False, integrity='missing')
            else:
                current = stamp(target)
                row.update(exists=True, actual_bytes=current['bytes'], readonly=core._readonly(target))
                if not old.get('managed'):
                    row['integrity'] = 'inactive'
                elif current == old.get('installed_file'):
                    row['integrity'] = 'intact'
                elif old.get('installed_file') is not None:
                    row['integrity'] = 'changed'
        except (OSError, ValueError) as error:
            row['verification_error'] = str(error)
        rows.append(row)
    managed = [r for r in rows if r.get('managed')]
    counts = {name + '_count': sum(r['integrity'] == name for r in managed)
              for name in ('intact', 'changed', 'missing', 'unverified')}
    counts['unprotected_count'] = sum(r['exists'] is True and r['readonly'] is False for r in managed)
    recorded = value['status']
    incomplete = recorded in ('installing', 'uninstalling', 'interrupted', 'error')
    issues = counts['changed_count'] + counts['missing_count'] + counts['unverified_count']
    actual_status = recorded if incomplete or not issues else 'needs_attention'
    message = ('No active texture pack.' if not managed else
               ('Installed file metadata has changed; rerun Install or use Verify.' if issues else
                f'All {len(managed)} installed files match their recorded copy metadata.'))
    if incomplete:
        message = str(value.get('error') or 'The previous operation did not complete.') + ' ' + message
    return dict(game_dir=str(paks.parent), paks_dir=str(paks), managed_count=len(managed),
                readonly_count=sum(r['readonly'] is True for r in managed), status=actual_status,
                recorded_status=recorded, message=message, archives=rows, manifest_path=str(path), **counts)
