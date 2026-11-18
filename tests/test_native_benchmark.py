"""Contract/correctness tests of real native binaries; no performance thresholds."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import unittest

MASK = (1 << 32) - 1


def query_oracle(count, queries, seed, hit_percent):
    state = seed

    def random32():
        nonlocal state
        state = (state * 1664525 + 1013904223) & MASK
        return state

    hits = queries * hit_percent // 100
    result = [(1 + random32() % count + (0 if i < hits else count), i < hits)
              for i in range(queries)]
    for i in range(queries, 1, -1):
        j = random32() % i
        result[i - 1], result[j] = result[j], result[i - 1]
    return result


def checksum_oracle(count, query_list, pure_offset=False, matched=False):
    offsets = {}
    position = 0
    for row in range(1, count + 1):
        offsets[row] = position
        position += len(f'{row},"value-{row}"\n'.encode())
    checksum = 14695981039346656037
    for row, hit in query_list:
        actual = bytes([int(hit)])
        if hit:
            actual += ((row * 3 + 7).to_bytes(8, 'little') if matched else
                       offsets[row].to_bytes(8, 'little') if pure_offset else f'value-{row}'.encode())
        for byte in actual:
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f'{checksum:016x}'


class NativeBenchmark(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='native-contract-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.schema = self.root / 'schema.json'
        self.schema.write_text('{"tables":[{"name":"records","columns":[{"name":"value","type":"string"}]}]}',
                               encoding='utf-8')
        self.counter = 0

    def invoke(self, *args, mode='bptree', data=None):
        self.counter += 1
        command = [str(EXE)]
        if SYSTEM == 'rdbms':
            command += ['--schema', str(self.schema), '--mode', mode, '--data-dir',
                        str(data or self.root / f'data-{self.counter}')]
        return subprocess.run(command + list(args), capture_output=True, text=True, timeout=30)

    def valid_run(self, count=9, queries=17, rounds=3, seed=0, hit=35, mode='bptree'):
        process = self.invoke('--count', str(count), '--queries', str(queries), '--rounds', str(rounds),
                              '--seed', str(seed), '--hit-percent', str(hit), mode=mode)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(process.stderr, '')
        value = json.loads(process.stdout)
        self.assertEqual(value['version'], 1)
        self.assertEqual(value['system'], SYSTEM)
        self.assertEqual(value['mode'], mode if SYSTEM == 'rdbms' else 'hash')
        self.assertEqual(value['config'], dict(count=count, queries=queries, rounds=rounds,
                                             seed=seed, hit_percent=hit))
        self.assertTrue(math.isfinite(value['setup_ms']) and value['setup_ms'] >= 0)
        self.assertEqual(len(value['rounds']), rounds)
        pure = SYSTEM != 'rdbms' or mode != 'scan'
        query_list = query_oracle(count, queries, seed, hit)
        for i, sample in enumerate(value['rounds']):
            self.assertEqual(sample['round'], i + 1)
            order = ['full_lookup', 'pure_lookup'] if pure else ['full_lookup']
            self.assertEqual(sample['order'], order[::-1] if pure and i % 2 else order)
            matched_order = order + ['matched_lookup'] if pure else order
            self.assertEqual(sample['matched_order'],
                             matched_order[::-1] if pure and i % 2 else matched_order)
            for layer in order + (['matched_lookup'] if pure else []):
                result = sample[layer]
                self.assertEqual(result['operations'], queries)
                self.assertEqual(result['hits'], queries * hit // 100)
                self.assertEqual(result['misses'], queries - queries * hit // 100)
                self.assertIs(result['verified'], True)
                self.assertEqual(result['checksum'], checksum_oracle(count, query_list,
                                 SYSTEM == 'rdbms' and layer == 'pure_lookup',
                                 matched=layer == 'matched_lookup'))
                self.assertTrue(math.isfinite(result['elapsed_ms']) and result['elapsed_ms'] >= 0)
            if not pure:
                self.assertIsNone(sample['pure_lookup'])
                self.assertIsNone(sample['matched_lookup'])
        metadata = value['metadata']
        for field in ['clock', 'build_flags', 'storage', 'flush', 'full_lookup_scope',
                      'compiler', 'platform', 'cache_policy', 'verification_scope']:
            self.assertTrue(metadata[field], field)
        self.assertEqual(metadata['build_info']['flags'], metadata['build_flags'])
        for field in ['compiler', 'flags', 'optimization', 'build_timestamp', 'source']:
            self.assertTrue(metadata['build_info'][field])
        self.assertIs(metadata['matched_structure_comparison'], pure)
        self.assertEqual(metadata['matched_lookup_scope'] is not None, pure)
        for field in ['matched_key_type', 'matched_value_type', 'matched_dataset']:
            if pure:
                self.assertTrue(metadata[field])
            else:
                self.assertIsNone(metadata[field])
        self.assertIs(metadata['equivalent_structure_comparison'], False)
        self.assertEqual(metadata['pure_lookup_scope'] is not None, pure)
        return value

    def test_partial_hits_exact_checksums_and_order(self):
        self.valid_run()

    def test_all_hits(self):
        self.valid_run(count=1, hit=100, seed=MASK)

    def test_all_misses(self):
        self.valid_run(count=1, hit=0)

    def test_rounding_and_smallest_query(self):
        self.valid_run(count=1, queries=1, rounds=1, hit=99)

    def test_seeded_repeated_processes(self):
        first = self.valid_run(seed=42)
        second = self.valid_run(seed=42)
        for a, b in zip(first['rounds'], second['rounds']):
            for layer in ['full_lookup', 'pure_lookup', 'matched_lookup']:
                self.assertEqual(a[layer]['checksum'], b[layer]['checksum'])

    def test_count_upper_bound(self):
        self.valid_run(count=10000, queries=7, rounds=1, hit=50)

    def test_invalid_options_never_emit_success(self):
        cases = [('--count', '0'), ('--count', '10001'), ('--queries', '0'),
                 ('--queries', '1000001'), ('--rounds', '0'), ('--rounds', '21'),
                 ('--seed', '4294967296'), ('--hit-percent', '101'),
                 ('--unknown', '1'), ('--count', '1', '--count', '2'), ('--count',)]
        cases += [('--count', text) for text in ['', '-1', '+1', ' 1', '1 ', '1.0', '1e2',
                                                'nan', 'inf', '1x', '99999999999999999999999999']]
        for args in cases:
            with self.subTest(args=args):
                process = self.invoke(*args)
                self.assertNotEqual(process.returncode, 0)
                self.assertEqual(process.stdout, '')
                self.assertTrue(process.stderr)

    def test_scan(self):
        if SYSTEM != 'rdbms':
            self.skipTest('RDBMS only')
        self.valid_run(mode='scan', hit=35)
        self.valid_run(mode='scan', hit=0, count=1)
        self.valid_run(mode='scan', hit=100, count=1)

    def test_directory_and_schema_safety(self):
        if SYSTEM != 'rdbms':
            self.skipTest('RDBMS only')
        data = self.root / 'existing'
        data.mkdir()
        sentinel = data / 'records.csv'
        sentinel.write_bytes(b'user data must survive\x00\xff')
        for selected in [data, sentinel, self.root / 'missing-parent' / 'new']:
            process = self.invoke('--count', '1', data=selected)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(process.stdout, '')
        self.assertEqual(sentinel.read_bytes(), b'user data must survive\x00\xff')
        self.schema.write_text('{"tables":[]}', encoding='utf-8')
        process = self.invoke('--count', '1')
        self.assertNotEqual(process.returncode, 0)
        self.assertEqual(process.stdout, '')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--system', choices=['redis', 'rdbms'], default='redis')
    options, remaining = parser.parse_known_args()
    EXE = Path(options.exe).resolve()
    SYSTEM = options.system
    unittest.main(argv=[__file__] + remaining)
