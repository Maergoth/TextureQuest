"""Committed texture reuse: genuine tiny D-only caches, no native/game execution."""
import copy
from contextlib import contextmanager, ExitStack
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import texturequest_channels as channels
import texturequest_dds_source as dds
import texturequest_graphics_proxy as graphics
import texturequest_setup as setup
import texturequest_upgrade as upgrade
from texture_swapper import core
from test_channels import fixture

ROOT = Path(os.environ['TQ_UNCHANGED_TEXTURE_TEST_ROOT']).absolute()
if ROOT.drive.casefold() != 'd:' or not ROOT.is_relative_to(Path('D:/eq2 paks')):
    raise RuntimeError('Require an owned D unchanged-texture test root')
ROOT.mkdir(parents=True, exist_ok=True)


class CommittedPreparation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game, self.source, _, _, self.choices, self.opener, _ = fixture(self.root, (4,))
        self.store = channels.ChannelStore(self.root / 'data')
        self.prepared = self.store.prepare(self.source, self.game, self.choices, opener=self.opener)
        self.pointer = self.store.state_dir / 'dds-prepared.json'
        self.object = self.store.objects / self.store.object_rows(self.prepared)[0]['entry']

    @contextmanager
    def no_dds_access(self):
        # Catch even a single per-object stat/open, rather than just hash calls.
        def guard(original):
            def checked(path, *args, **kwargs):
                if path.suffix.casefold() == '.dds' and path.is_relative_to(self.store.objects):
                    raise AssertionError('Unexpected DDS object access: ' + str(path))
                return original(path, *args, **kwargs)
            return checked
        with ExitStack() as stack:
            for name in ('stat', 'lstat', 'open'):
                stack.enter_context(patch.object(Path, name, guard(getattr(Path, name))))
            yield

    def commit(self, value):
        core._save(self.pointer, value)
        return value

    def corrupt_dds(self):
        raw = self.object.read_bytes()
        self.object.write_bytes(raw[:-1] + bytes([raw[-1] ^ 1]))

    def test_persistent_reuse_by_new_store_instances_never_touches_dds(self):
        with self.no_dds_access(), patch.object(dds.DDSStore, 'verify', side_effect=AssertionError('Full scan')):
            for factory in (dds.DDSStore, channels.ChannelStore, dds.DDSStore):
                self.assertEqual(factory(self.store.state_dir).verify_for_install(self.prepared), self.prepared)

    def test_cached_admission_is_not_fresh_byte_proof_but_manual_verify_is(self):
        self.corrupt_dds()
        with self.no_dds_access():
            self.store.verify_for_install(self.prepared)
        with self.assertRaisesRegex(ValueError, 'checksum'):
            self.store.verify(self.prepared, full=True)

    def test_missing_receipt_and_different_texture_version_use_full_verification(self):
        self.corrupt_dds()
        changed = dict(self.prepared, texture_version='1.3.1')
        with patch.object(self.store, 'verify', wraps=self.store.verify) as verify:
            with self.assertRaisesRegex(ValueError, 'checksum'):
                self.store.verify_for_install(changed)
            verify.assert_called_once_with(changed, full=True)
        self.pointer.unlink()
        with patch.object(self.store, 'verify', wraps=self.store.verify) as verify:
            with self.assertRaisesRegex(ValueError, 'checksum'):
                self.store.verify_for_install(self.prepared)
            verify.assert_called_once_with(self.prepared, full=True)

    def test_legacy_receipt_uses_full_verification(self):
        legacy = self.commit(dict(self.prepared, distribution='legacy-full-dds'))
        self.corrupt_dds()
        with patch.object(self.store, 'verify', wraps=self.store.verify) as verify:
            with self.assertRaisesRegex(ValueError, 'checksum'):
                self.store.verify_for_install(legacy)
            verify.assert_called_once_with(legacy, full=True)

    def test_same_version_changed_identities_refuse_without_scanning(self):
        variants = [dict(self.prepared, **{key: 'e' * 64}) for key in
                    ('source_sha256', 'index_sha256', 'choices_sha256', 'record_sha256')]
        variants += [dict(self.prepared, game=str(self.root / 'other')),
                     dict(self.prepared, object_root=str(self.root / 'other')),
                     dict(self.prepared, object_count=self.prepared['object_count'] + 1)]
        for key in ('objects', 'map', 'coverage'):
            value = copy.deepcopy(self.prepared); value[key]['sha256'] = 'e' * 64
            variants.append(value)
        with self.no_dds_access(), patch.object(self.store, 'verify', side_effect=AssertionError('Full scan')):
            for value in variants:
                with self.subTest(identity=value), self.assertRaisesRegex(ValueError, 'Verify / Update'):
                    self.store.verify_for_install(value)

    def test_changed_committed_metadata_refuses_without_scanning(self):
        for key in ('objects', 'map'):
            path = Path(self.prepared[key]['path']); raw = path.read_bytes()
            path.write_bytes(raw[:-1] + bytes([raw[-1] ^ 1]))
            with self.no_dds_access(), self.assertRaisesRegex(ValueError, 'Verify / Update'):
                self.store.verify_for_install(self.prepared)
            path.write_bytes(raw)
        self.pointer.write_bytes(b'{}')
        with self.no_dds_access(), self.assertRaisesRegex(ValueError, 'Verify / Update'):
            self.store.verify_for_install(self.prepared)

    def test_map_header_and_metadata_bounds_are_checked_without_dds_access(self):
        from texturequest_texture_map import MAX_BYTES
        path = Path(self.prepared['map']['path']); raw = path.read_bytes()
        for offset in (0, 8, 20):  # Magic, texture version, canonical source digest.
            changed = bytearray(raw); changed[offset] ^= 1; path.write_bytes(changed)
            prepared = copy.deepcopy(self.prepared); prepared['map']['sha256'] = dds.sha(changed)
            self.commit(prepared)
            with self.no_dds_access(), self.assertRaisesRegex(ValueError, 'Verify / Update'):
                self.store.verify_for_install(prepared)
        path.write_bytes(raw)
        for key, size in (('map', MAX_BYTES + 1), ('objects', dds.MAX_INDEX + 1)):
            prepared = copy.deepcopy(self.prepared); prepared[key]['bytes'] = size
            self.commit(prepared)
            with self.no_dds_access(), self.assertRaisesRegex(ValueError, 'Verify / Update'):
                self.store.verify_for_install(prepared)

    def test_preparation_does_not_commit_over_corrupt_existing_dds(self):
        self.corrupt_dds(); before = self.pointer.read_bytes()
        with self.assertRaises(ValueError):
            self.store.prepare(self.source, self.game, self.choices, opener=self.opener)
        self.assertEqual(self.pointer.read_bytes(), before)

    def test_all_three_automatic_handoff_stages_reuse_without_dds_access(self):
        manager = graphics.GraphicsManager(self.store.state_dir, runtime_dir=self.root / 'unused-runtime')
        ui = MagicMock()
        ui.status.return_value = dict(installed=True, installed_newer_app=False)
        ui.legacy_in_use.return_value = False; ui._all_games.return_value = []
        migration = upgrade.Upgrade(self.store.state_dir, ui)
        def activate(game, prepared, **kwargs):
            with core._lock(self.store.state_dir / 'operation.lock'), core._lock(self.store.state_dir / 'sideload/helper-owner.lock'):
                return manager._activate_prepared(game, prepared)
        ui.install.side_effect = activate
        calls = []; original = dds.DDSStore.verify_for_install
        def admitted(owner, prepared):
            calls.append(owner); return original(owner, prepared)
        with self.no_dds_access(), patch.object(setup, 'ensure_closed'), patch.object(graphics, 'ensure_closed'), \
             patch.object(upgrade, 'ensure_closed'), patch.object(migration, 'snapshot', return_value={}), \
             patch.object(migration, 'before'), patch.object(self.store, 'prepare', side_effect=AssertionError('Rescan')), \
             patch.object(dds.DDSStore, 'verify', side_effect=AssertionError('Full scan')), \
             patch.object(dds.DDSStore, 'verify_for_install', autospec=True, side_effect=admitted):
            _, prepared = setup.prepare_graphics(self.game, lambda **_: self.source, self.choices,
                self.store, ui, migration, minimum='1.3.0', binding={}, in_use=lambda: False, upgrading=True)
        self.assertEqual(len(calls), 3)
        self.assertEqual(prepared, self.prepared)
        self.assertEqual((manager.base / 'texturequest-map.bin').read_bytes(), Path(prepared['map']['path']).read_bytes())
        self.assertEqual(core._load(migration.receipt(self.game))['status'], 'complete')


if __name__ == '__main__': unittest.main(verbosity=2)
