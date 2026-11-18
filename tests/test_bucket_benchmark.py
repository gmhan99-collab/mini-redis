"""Bucket CLI/native/real HTTP contract checks; timings are never fixed expectations."""
import argparse
from collections import Counter
from contextlib import closing
import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/server'))
from test_functional import Server

class BucketTests(unittest.TestCase):
    def native(self, *options):
        result = subprocess.run([str(self.exe), *map(str, options)], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_clock_properties(self):
        import math
        import os
        for extra in [[], ['--prepare-only']]:
            metadata = self.native('--count', 1, '--queries', 1, '--rounds', 1, *extra)['metadata']
            if os.name == 'nt':
                import ctypes
                frequency = ctypes.c_longlong()
                self.assertTrue(ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(frequency)))
                expected_frequency = frequency.value
                expected_resolution = 1000 / expected_frequency
            else:
                expected_frequency = 1_000_000_000  # measurement_ticks nanosecond encoding
                expected_resolution = time.clock_getres(time.CLOCK_MONOTONIC) * 1000
            self.assertEqual(metadata['clock_frequency_hz'], expected_frequency)
            self.assertTrue(math.isclose(metadata['clock_resolution_ms'], expected_resolution, rel_tol=1e-11))
            self.assertEqual(metadata['clock_pair_delta_ms']['samples'], 256)
            self.assertGreaterEqual(metadata['clock_pair_delta_ms']['min'], 0)

    def test_invalid_options(self):
        for options in [('--count', '0'), ('--queries', '1000001'), ('--rounds', '21'),
                        ('--count', '10001'), ('--seed', '4294967296'), ('--hit-percent', '101'),
                        ('--initial-buckets', '3'), ('--hash-seed', '00'),
                        ('--bucket-structure', 'tree'), ('--distribution', 'constant'),
                        ('--prepare-only', '--prepare-only'), ('--count',), ('--count', '1', '--count', '2')]:
            with self.subTest(options=options):
                result = subprocess.run([str(self.exe), *options], capture_output=True, text=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(result.stdout)

    def test_native_same_data_and_verified_structure(self):
        for distribution in ['normal', 'collision']:
            prepared = []
            results = []
            for mode in ['list', 'rbtree']:
                options = ['--count', '150', '--queries', '301', '--rounds', '2', '--seed', '4294967295',
                           '--hit-percent', '37', '--bucket-structure', mode,
                           '--initial-buckets', '1', '--distribution', distribution]
                prepared.append(self.native(*options, '--prepare-only'))
                result = self.native(*options)
                results.append(result)
                for row in result['rounds']:
                    self.assertTrue(row['verified'])
                    self.assertEqual(row['bucket_structure'], mode)
                    self.assertEqual(row['hits'], 301 * 37 // 100)
                    self.assertEqual(row['hits'] + row['misses'], 301)
                    stats = row['stats']
                    self.assertEqual(stats['size'], 150)
                    self.assertEqual(stats['bucket_count'], 256)
                    self.assertEqual(stats['rehash_count'], 8)
                    self.assertEqual(sum(stats['bucket_size_histogram']), 256)
                    self.assertEqual(sum(i * n for i, n in enumerate(stats['bucket_size_histogram'])), 150)
                    self.assertEqual(sum(s['rehash'] for s in row['insertion_samples']), 8)
                    self.assertEqual(len(row['insertion_samples']), 150)
                    self.assertTrue(all(s['ok'] and s['elapsed_ms'] >= 0 for s in row['insertion_samples']))
                    self.assertGreaterEqual(row['insertion_ms'], 0)
                    self.assertGreaterEqual(row['lookup_ms'], 0)
                    if distribution == 'collision':
                        self.assertTrue(result['metadata']['initial_concentrated'])
                        self.assertTrue(result['metadata']['final_concentrated'])
                        self.assertEqual(stats['max_bucket_size'], 150)
                        if mode == 'list':
                            self.assertEqual(stats['max_chain_length'], 150)
                            self.assertEqual(stats['max_tree_height'], 0)
                        else:
                            self.assertEqual(stats['max_chain_length'], 0)
                            self.assertGreater(stats['max_tree_height'], 1)
                            self.assertLessEqual(stats['max_tree_height'], 16)
            for field in ['keys', 'values', 'query_ids']:
                self.assertEqual(prepared[0][field], prepared[1][field])
            self.assertEqual(len(set(prepared[0]['keys'])), 300)
            self.assertEqual(sum(i < 150 for i in prepared[0]['query_ids']), 301 * 37 // 100)
            self.assertEqual(results[0]['rounds'][0]['checksum'], results[1]['rounds'][0]['checksum'])

    def test_hit_extremes(self):
        for percent in [0, 100]:
            result = self.native('--count', 1, '--queries', 10, '--rounds', 1, '--hit-percent', percent)
            self.assertEqual(result['rounds'][0]['hits'], percent // 10)

    def test_server_options_health_collision_rehash_ttl(self):
        prepared = self.native('--count', 100, '--queries', 101, '--seed', 42, '--distribution', 'collision',
                               '--initial-buckets', 1, '--prepare-only')
        for mode in ['list', 'rbtree']:
            with closing(socket.socket()) as probe:
                probe.bind(('127.0.0.1', 0))
                port = probe.getsockname()[1]
            server = Server(self.server, port, ['--bucket-structure', mode, '--buckets', '1',
                                              '--hash-seed', prepared['config']['hash_seed']])
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
            def request(method, path, body=None, headers=None):
                connection.request(method, path, body=body, headers=headers or {})
                response = connection.getresponse()
                return response.status, response.read()
            try:
                status, body = request('GET', '/health')
                health = json.loads(body)
                self.assertEqual(status, 200)
                self.assertEqual(health['bucket_structure'], mode)
                self.assertEqual(health['bucket_support'], ['list', 'rbtree'])
                self.assertEqual(health['config']['hash_seed'], prepared['config']['hash_seed'])
                self.assertEqual(health['config']['seed_source'], 'explicit')
                self.assertTrue(health['build_info'])
                for key, value in zip(prepared['keys'][:100], prepared['values'][:100]):
                    self.assertEqual(request('PUT', '/keys/' + key, value)[0], 200)
                for index in prepared['query_ids']:
                    status, body = request('GET', '/keys/' + prepared['keys'][index])
                    self.assertEqual(status, 200 if index < 100 else 404)
                    if index < 100:
                        self.assertEqual(body.decode(), prepared['values'][index])
                health = json.loads(request('GET', '/health')[1])
                stats = health['stats']
                self.assertEqual(stats['bucket_count'], 128)
                self.assertEqual(stats['max_bucket_size'], 100)
                self.assertEqual(stats['nonempty_buckets'], 1)
                self.assertEqual(stats['rehash_count'], 7)
                key = prepared['keys'][0]
                self.assertEqual(request('PUT', '/keys/' + key, 'replacement', {'X-TTL-Ms': '20'})[0], 200)
                time.sleep(.04)
                self.assertEqual(json.loads(request('GET', '/keys/' + key)[1]), {'error': 'expired'})
                for key in prepared['keys'][1:100]:
                    self.assertEqual(request('DELETE', '/keys/' + key)[0], 200)
                self.assertEqual(json.loads(request('GET', '/health')[1])['stats']['size'], 0)
            finally:
                connection.close()
                server.stop()

    def test_server_invalid_options_and_legacy_default(self):
        for options in [('--bucket-structure', 'bad'), ('--buckets', '0'), ('--hash-seed', '00'),
                        ('--buckets', '65537'), ('--buckets', '1', '--buckets', '2'), ('0',)]:
            result = subprocess.run([str(self.server), *options], capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
        with closing(socket.socket()) as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        server = Server(self.server, port)
        try:
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
            connection.request('GET', '/health')
            health = json.loads(connection.getresponse().read())
            connection.close()
            self.assertEqual(health['config']['initial_buckets'], 1024)
            self.assertEqual(health['config']['seed_source'], 'os_random')
            self.assertEqual(health['bucket_structure'], 'list')
            self.assertEqual(len(health['config']['hash_seed']), 32)
        finally:
            server.stop()

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--server', required=True)
    args = parser.parse_args()
    BucketTests.exe = Path(args.exe).resolve()
    BucketTests.server = Path(args.server).resolve()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
