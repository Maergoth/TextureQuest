"""Bounded chunk pipeline using tiny generated-channel/game fixtures on D:."""
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.request import Request
import zipfile

APP = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(APP / 'src'))
import texturequest_channels as channels
import texturequest_channel_codec as codec
from texture_swapper import core
from texturequest_release import Paused
from test_channels import fixture
from test_direct import Response
from test_texture_map import vpk


class StreamingChannels(unittest.TestCase):
    def setUp(self):
        (APP / 'build').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=APP / 'build')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game, self.source, self.index, self.examples, self.choices, _, _ = fixture(self.root, families=(4, 4, 4))
        self.store = channels.ChannelStore(self.root / 'data')
        self.requests = []
        self.chunks = {}
        pins = []
        # Each genuine existing fixture recipe gets one small transport chunk.
        # The production index validator still checks the complete graph.
        for n, example in enumerate(self.examples, 1):
            payload = example[2]
            packed = io.BytesIO()
            with zipfile.ZipFile(packed, 'w', zipfile.ZIP_DEFLATED) as archive:
                info = zipfile.ZipInfo('channels/' + codec.sha(payload) + '.tqc')
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                archive.writestr(info, payload)
            raw = packed.getvalue()
            name = f'channels-{n:03d}-' + codec.sha(raw)[:12] + '.zip'
            self.chunks[name] = raw
            pin = dict(name=name, bytes=len(raw), sha256=codec.sha(raw), objects=1,
                       url=channels.REPOSITORY + '/releases/download/v1.3.0/' + name)
            pins.append(pin)
            obj = self.index['objects'][n - 1]
            obj['chunk'] = name
            self.index['recipes'][n - 1]['payload'] = obj
        self.source['chunks'] = pins
        self.source['compressed_download_bytes'] = sum(p['bytes'] for p in pins)
        self.seal_index()

    def seal_index(self):
        self.index_raw = json.dumps(self.index).encode()
        self.source['index']['bytes'] = len(self.index_raw)
        self.source['index']['sha256'] = codec.sha(self.index_raw)

    def opener(self, request, **kwargs):
        url = request.full_url
        self.requests.append(url)
        raw = self.index_raw if url.endswith('channel-index.json') else self.chunks[url.rsplit('/', 1)[-1]]
        start = int(request.headers.get('Range', 'bytes=0-').split('=')[1].split('-')[0])
        return Response(raw, start=start, status=206 if start else 200)

    def path(self, n):
        return self.store.objects / 'dds' / (self.index['recipes'][n]['object_sha256'] + '.dds')

    def prime(self, *numbers):
        (self.store.objects / 'dds').mkdir(parents=True, exist_ok=True)
        for n in numbers:
            self.path(n).write_bytes(self.examples[n][1])

    def previous_receipt(self):
        # A distinct checksummed older receipt is a publication sentinel. It is
        # never used as proof of cache readiness or game/runtime compatibility.
        self.store.state_dir.mkdir(parents=True, exist_ok=True)
        pointer = self.store.state_dir / 'dds-prepared.json'
        core._save(pointer, dict(schema='texturequest-loose-dds-prepared-v1', texture_version='1.2.9',
                                 source_sha256='1' * 64, sentinel='preserve until successful publication'))
        return pointer, pointer.read_bytes()

    def prepare(self, **kwargs):
        return self.store.prepare(self.source, self.game, self.choices, opener=kwargs.pop('opener', self.opener), **kwargs)

    def zip_requests(self):
        return [u for u in self.requests if u.endswith('.zip')]

    def test_first_transfer_overlaps_remaining_diff_and_reuses_unchanged_outputs(self):
        self.prime(1, 2)
        started, last_diff = threading.Event(), threading.Event()
        source_checked = threading.Event()
        events = []
        original_pin, original_sources = channels.pin_file, channels.read_sources
        def sources(*args):
            value = original_sources(*args)
            source_checked.set()
            return value
        def opener(request, **kwargs):
            if request.full_url.endswith('.zip'):
                self.assertTrue(source_checked.is_set())
                events.append('download')
                started.set()
                self.assertTrue(last_diff.wait(5), 'remaining diff never ran while transfer was pending')
            return self.opener(request, **kwargs)
        def pin(path, identity):
            if path == self.path(2):
                self.assertTrue(started.wait(5), 'download waited for the whole cache census')
                events.append('last-local-diff')
                last_diff.set()
            return original_pin(path, identity)
        before = (self.game / 'paks/test.vpk').read_bytes()
        with patch.object(channels, 'pin_file', side_effect=pin), patch.object(channels, 'read_sources', side_effect=sources):
            prepared = self.prepare(opener=opener)
        self.assertEqual(events[:2], ['download', 'last-local-diff'])
        self.assertEqual(len(self.zip_requests()), 1)
        self.assertTrue(self.zip_requests()[0].endswith(self.source['chunks'][0]['name']))
        self.assertEqual(prepared['coverage']['mapped_groups'], 3)
        for n, example in enumerate(self.examples):
            self.assertEqual(self.path(n).read_bytes(), example[1])
        self.assertEqual((self.game / 'paks/test.vpk').read_bytes(), before)

    def test_fully_unchanged_cache_never_downloads_a_chunk_or_reconstructs(self):
        self.prime(0, 1, 2)
        with patch.object(codec.LocalAssembler, 'assemble', side_effect=AssertionError('unchanged output reconstructed')):
            prepared = self.prepare()
        self.assertEqual(self.zip_requests(), [])
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(prepared['coverage']['mapped_groups'], 3)

    def test_pause_during_overlap_preserves_previous_receipt_and_owned_resume_history(self):
        self.prime(1, 2)
        pointer, before = self.previous_receipt()
        started, release_read, paused = threading.Event(), threading.Event(), threading.Event()
        original_pin = channels.pin_file
        class WaitingResponse(Response):
            def read(owner, size=-1):
                self.assertTrue(release_read.wait(5))
                return super(WaitingResponse, owner).read(size)
        def opener(request, **kwargs):
            if request.full_url.endswith('.zip'):
                self.requests.append(request.full_url)
                started.set()
                return WaitingResponse(self.chunks[request.full_url.rsplit('/', 1)[-1]], status=200)
            return self.opener(request, **kwargs)
        def pin(path, identity):
            if path == self.path(2):
                self.assertTrue(started.wait(5))
                paused.set()
                release_read.set()
            return original_pin(path, identity)
        with patch.object(channels, 'pin_file', side_effect=pin):
            with self.assertRaises(Paused):
                self.prepare(opener=opener, pause=paused.is_set)
        self.assertEqual(pointer.read_bytes(), before)
        self.assertFalse(self.path(0).exists())
        self.assertTrue(list((self.store.base / 'downloads').glob('*.pending.json')))
        self.assertEqual(len(self.zip_requests()), 1)
        with core._lock(self.store.state_dir / 'operation.lock'):
            pass

    def test_download_error_preserves_cache_and_receipt_without_later_transfer(self):
        self.prime(1, 2)
        pointer, before = self.previous_receipt()
        started, last_diff = threading.Event(), threading.Event()
        original_pin = channels.pin_file
        def opener(request, **kwargs):
            if request.full_url.endswith('.zip'):
                self.requests.append(request.full_url)
                started.set()
                self.assertTrue(last_diff.wait(5))
                raise OSError('invented transfer interruption')
            return self.opener(request, **kwargs)
        def pin(path, identity):
            if path == self.path(2):
                self.assertTrue(started.wait(5))
                last_diff.set()
            return original_pin(path, identity)
        with patch.object(channels, 'pin_file', side_effect=pin):
            with self.assertRaisesRegex(OSError, 'invented transfer interruption'):
                self.prepare(opener=opener)
        self.assertEqual(pointer.read_bytes(), before)
        self.assertEqual(len(self.zip_requests()), 1)
        for n in (1, 2):
            self.assertEqual(self.path(n).read_bytes(), self.examples[n][1])

    def test_corrupt_cached_output_cancels_and_joins_inflight_before_locks_release(self):
        self.prime(1)
        corrupt = self.path(1).read_bytes()[:-1] + b'?'
        self.path(1).write_bytes(corrupt)
        pointer, before = self.previous_receipt()
        started, worker_stopped = threading.Event(), threading.Event()
        original_pin, original_download = channels.pin_file, channels._download
        worker_pause = None
        class WaitingResponse(Response):
            def read(owner, size=-1):
                # Only the owned cancellation callback is polled, with an event
                # timeout guarding the invented leaf against a test deadlock.
                for _ in range(500):
                    if worker_pause():
                        raise Paused('owned transfer cancelled')
                    worker_stopped.wait(.01)
                raise AssertionError('owned transfer was not cancelled')
        def download(*args, **kwargs):
            nonlocal worker_pause
            worker_pause = kwargs['pause']
            try:
                return original_download(*args, **kwargs)
            finally:
                worker_stopped.set()
        def opener(request, **kwargs):
            if request.full_url.endswith('.zip'):
                self.requests.append(request.full_url)
                started.set()
                return WaitingResponse(self.chunks[request.full_url.rsplit('/', 1)[-1]], status=200)
            return self.opener(request, **kwargs)
        def pin(path, identity):
            if path == self.path(1):
                self.assertTrue(started.wait(5))
            return original_pin(path, identity)
        with patch.object(channels, '_download', side_effect=download), patch.object(channels, 'pin_file', side_effect=pin):
            with self.assertRaisesRegex(ValueError, 'checksum'):
                self.prepare(opener=opener)
        self.assertTrue(worker_stopped.is_set())
        self.assertEqual(self.path(1).read_bytes(), corrupt)
        self.assertEqual(pointer.read_bytes(), before)
        self.assertEqual(len(self.zip_requests()), 1)
        with core._lock(self.store.state_dir / 'operation.lock'):
            pass

    def test_invalid_published_channel_ownership_refuses_before_any_chunk(self):
        pointer, before = self.previous_receipt()
        self.index['recipes'][0]['generated_channels'] = ['A']
        self.seal_index()
        with self.assertRaisesRegex(ValueError, 'Channel ownership'):
            self.prepare()
        self.assertEqual(self.zip_requests(), [])
        self.assertEqual(pointer.read_bytes(), before)

    def test_unmatched_local_original_never_queues_its_chunk(self):
        self.prime(1, 2)
        originals = {example[3]['member']: example[0] for example in self.examples[1:]}
        (self.game / 'paks/test.vpk').write_bytes(vpk(originals))
        prepared = self.prepare()
        self.assertEqual(self.zip_requests(), [])
        self.assertEqual(prepared['coverage']['selected_groups'], 3)
        self.assertEqual(prepared['coverage']['mapped_groups'], 2)
        self.assertEqual(prepared['coverage']['pending_groups'], 1)

    def test_corrupt_transport_cannot_publish_or_replace_previously_cached_files(self):
        self.prime(1, 2)
        pointer, before = self.previous_receipt()
        normal = self.opener
        def opener(request, **kwargs):
            response = normal(request, **kwargs)
            if request.full_url.endswith('.zip'):
                raw = response.getvalue()
                return Response(raw[:-1] + bytes([raw[-1] ^ 1]), status=200)
            return response
        with patch.object(codec.LocalAssembler, 'assemble', side_effect=AssertionError('unauthenticated payload used')):
            with self.assertRaises(ValueError):
                self.prepare(opener=opener)
        self.assertEqual(pointer.read_bytes(), before)
        self.assertFalse(self.path(0).exists())
        self.assertEqual(len(self.zip_requests()), 1)
        for n in (1, 2):
            self.assertEqual(self.path(n).read_bytes(), self.examples[n][1])

    def test_scan_failure_blocks_a_late_worker_response_open(self):
        self.prime(1)
        self.path(1).write_bytes(self.path(1).read_bytes()[:-1] + b'?')
        entered, stopped = threading.Event(), threading.Event()
        original_pin = channels.pin_file
        def download(row, folder, *, notify, pause, opener):
            entered.set()
            for _ in range(500):
                if pause():
                    with self.assertRaises(Paused):
                        opener(Request(row['url']))
                    stopped.set()
                    raise Paused('owned cancellation confirmed')
                stopped.wait(.01)
            raise AssertionError('scan failure did not cancel owned worker')
        def pin(path, identity):
            if path == self.path(1):
                self.assertTrue(entered.wait(5))
            return original_pin(path, identity)
        with patch.object(channels, '_download', side_effect=download), patch.object(channels, 'pin_file', side_effect=pin):
            with self.assertRaisesRegex(ValueError, 'checksum'):
                self.prepare()
        self.assertTrue(stopped.is_set())
        self.assertEqual(self.zip_requests(), [])

    def test_multiple_missing_chunks_keep_one_transfer_and_one_source_bundle(self):
        original_sources, original_download = channels.read_sources, channels._download
        original_assemble = codec.LocalAssembler.assemble
        current_transfer = 0
        maximum_transfers = 0
        unassembled_sources = set()
        def sources(*args):
            self.assertFalse(unassembled_sources, 'previous source bundle retained while reading another')
            values = original_sources(*args)
            unassembled_sources.update(values)
            return values
        def download(*args, **kwargs):
            nonlocal current_transfer, maximum_transfers
            current_transfer += 1
            maximum_transfers = max(maximum_transfers, current_transfer)
            try:
                return original_download(*args, **kwargs)
            finally:
                current_transfer -= 1
        def assemble(owner, original, payload, recipe):
            raw = original_assemble(owner, original, payload, recipe)
            unassembled_sources.remove(recipe['id'])
            return raw
        with patch.object(channels, 'read_sources', side_effect=sources), \
             patch.object(channels, '_download', side_effect=download), \
             patch.object(codec.LocalAssembler, 'assemble', autospec=True, side_effect=assemble):
            prepared = self.prepare()
        self.assertEqual(maximum_transfers, 1)
        self.assertEqual(current_transfer, 0)
        self.assertFalse(unassembled_sources)
        self.assertEqual(len(self.zip_requests()), 3)
        self.assertEqual(prepared['coverage']['mapped_groups'], 3)
        for n, example in enumerate(self.examples):
            self.assertEqual(self.path(n).read_bytes(), example[1])


if __name__ == '__main__':
    unittest.main()
