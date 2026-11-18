"""Cross-engine matching and real Make metadata invalidation; stdlib only."""
from contextlib import closing
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RDBMS = ROOT.parent / 'hjm-mini-rdbms'
sys.path.insert(0, str(ROOT / 'tests/server'))
sys.path.insert(0, str(RDBMS / 'tests'))
from test_functional import Server
from http_integration import RdbServer
from test_native_benchmark import query_oracle, checksum_oracle


def execute(command, cwd=None, env=None):
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise AssertionError((command, result.returncode, result.stdout, result.stderr))
    return result


def native(executable, count=19, queries=53, hit=41, seed=42):
    with tempfile.TemporaryDirectory(prefix='matched-') as directory:
        root = Path(directory)
        args = [str(executable), '--count', str(count), '--queries', str(queries),
                '--rounds', '3', '--seed', str(seed), '--hit-percent', str(hit)]
        if 'rdbms' in executable.name:
            schema = root / 'schema.json'
            schema.write_text('{"tables":[{"name":"records","columns":[{"name":"value","type":"string"}]}]}')
            args += ['--schema', str(schema), '--data-dir', str(root / 'data')]
        return json.loads(execute(args).stdout)


def health(executable):
    with tempfile.TemporaryDirectory(prefix='metadata-health-') as directory:
        if 'rdbms' in executable.name:
            server = RdbServer(executable, Path(directory))
        else:
            with closing(socket.socket()) as probe:
                probe.bind(('127.0.0.1', 0))
                port = probe.getsockname()[1]
            server = Server(executable, port)
        try:
            with closing(http.client.HTTPConnection('127.0.0.1', server.port, timeout=10)) as client:
                client.request('GET', '/health')
                response = client.getresponse()
                body = json.loads(response.read())
                assert response.status == 200, body
                assert response.getheader('Server-Timing') is None
                client.request('GET', '/health')
                again = client.getresponse()
                assert json.loads(again.read()) == body
                return body['build_info']
        finally:
            server.stop()


class RemainingMeasurement(unittest.TestCase):
    def test_actual_matched_layers_have_same_independent_checksum(self):
        for hit, seed in [(0, 0), (41, 42), (100, 4294967295)]:
            values = [native(ROOT / 'build/todo-remaining/mini-redis-benchmark.exe', hit=hit, seed=seed),
                      native(RDBMS / 'build/todo-remaining/mini-rdbms-benchmark.exe', hit=hit, seed=seed)]
            expected = checksum_oracle(19, query_oracle(19, 53, seed, hit), matched=True)
            for value in values:
                self.assertIs(value['metadata']['equivalent_structure_comparison'], False)
                self.assertIs(value['metadata']['matched_structure_comparison'], True)
                for result in value['rounds']:
                    self.assertEqual(result['matched_lookup']['checksum'], expected)

    def test_generated_header_escapes_arbitrary_flag_text_and_preserves_mtime(self):
        flag_cases = [
            '-O0 -O3 -DNOTE="two words\\path %s */"',
            '-O0 -O3 -DTAG="\U0001f600 \ud55c\uae00"',
            '-O0 -O3 -DQ="??/ ??= ??\' ??( ??) ??! ??< ??> ??-"',
            '-O0 -O3 -DNOTE="\t\n\r\x01\x1f \\"',
        ]
        with tempfile.TemporaryDirectory(prefix='build-info-') as directory:
            root = Path(directory)
            header = root / 'build_info.h'
            program = root / 'metadata.c'
            program.write_text('#include <stdio.h>\n#include "build_info.h"\n'
                               'int main(void) { puts(BUILD_INFO_JSON); '
                               'const unsigned char *p = (const unsigned char *)BENCH_BUILD_FLAGS; '
                               'while (*p) { printf("%02x", (unsigned)*p++); } return 0; }\n')
            executable = root / 'metadata.exe'
            command = [sys.executable, str(ROOT / 'tools/generate_build_info.py'), str(header), 'server']
            for flags in flag_cases:
                with self.subTest(flags=flags):
                    env = os.environ | dict(MEASUREMENT_CC='gcc', MEASUREMENT_SERVER_FLAGS=flags,
                        MEASUREMENT_SERVER_SOURCE='quotes " backslash \\ emoji \U0001f600 Hangul \ud55c\uae00 ??/')
                    execute(command, env=env)
                    original = header.stat().st_mtime_ns
                    execute(command, env=env)
                    self.assertEqual(header.stat().st_mtime_ns, original)
                    execute(['gcc', '-std=c11', '-trigraphs', '-Werror', str(program), '-o', str(executable)])
                    json_text, flag_hex = execute([str(executable)]).stdout.split('\n', 1)
                    metadata = json.loads(json_text)
                    self.assertEqual(metadata['flags'], flags)
                    self.assertEqual(bytes.fromhex(flag_hex), flags.encode('utf-8'))
                    self.assertEqual(metadata['optimization'], '-O3')
                    self.assertEqual(metadata['source'], env['MEASUREMENT_SERVER_SOURCE'])
                    self.assertTrue(metadata['build_timestamp'])

    def test_make_rebuilds_binaries_when_flags_change_and_keeps_unchanged_builds(self):
        for project, server_name, native_name, base in [
            (ROOT, 'mini-redis.exe', 'mini-redis-benchmark.exe', '-std=c11 -Wall -Wextra'),
            (RDBMS, 'mini-rdbms-server.exe', 'mini-rdbms-benchmark.exe',
             '-std=c11 -Wall -Wextra -Wpedantic -D_FILE_OFFSET_BITS=64 -Iinclude'),
        ]:
            relative = 'build/todo-remaining/metadata-regression'
            build = project / relative
            build.mkdir(parents=True, exist_ok=True)
            previous = None
            for optimization in ['-O0', '-O3']:
                flags = base + ' ' + optimization + ' -DMEASUREMENT_LABEL="two words"'
                command = ['mingw32-make', 'server', 'benchmark', 'BUILD_DIR=' + relative,
                           'CFLAGS=' + flags, 'BENCH_CFLAGS=' + flags]
                result = execute(command, cwd=project)
                (build / ('build-O0.log' if optimization == '-O0' else 'build-O3.log')).write_text(
                    result.stdout + result.stderr, encoding='utf-8')
                mtimes = [(build / name).stat().st_mtime_ns for name in [server_name, native_name]]
                if previous:
                    self.assertNotEqual(mtimes, previous)
                server_info = health(build / server_name)
                native_value = native(build / native_name)
                for info in [server_info, native_value['metadata']['build_info']]:
                    self.assertIn(flags, info['flags'])
                    self.assertEqual(info['optimization'], optimization.split()[0])
                    self.assertIn('two words', info['flags'])
                expected = checksum_oracle(19, query_oracle(19, 53, 42, 41), matched=True)
                for round_result in native_value['rounds']:
                    self.assertEqual(round_result['matched_lookup']['checksum'], expected)
                execute(command, cwd=project)
                self.assertEqual(mtimes, [(build / name).stat().st_mtime_ns
                                         for name in [server_name, native_name]])
                previous = mtimes


if __name__ == '__main__':
    unittest.main()
