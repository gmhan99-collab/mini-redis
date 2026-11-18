"""Real-server checks on Windows and POSIX, using only the standard library."""
import argparse
import concurrent.futures
from contextlib import closing, ExitStack
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest


def assert_port_released(port):
    with socket.socket() as rebound:
        if os.name != 'nt':
            # Match the server's bind options so accepted sockets in TIME_WAIT do not mask release.
            rebound.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        rebound.bind(('127.0.0.1', port))
        rebound.listen()


class RawClient:
    def __init__(self, port):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=4)
        self.reader = self.socket.makefile('rb')

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.reader.close()
        self.socket.close()

    def response(self):
        status_line = self.reader.readline(4096)
        if not status_line:
            raise AssertionError('EOF before response')
        status = int(status_line.split()[1])
        headers = {}
        while True:
            line = self.reader.readline(8192)
            if line == b'\r\n':
                break
            if not line:
                raise AssertionError('EOF in headers')
            name, value = line.decode('ascii').split(':', 1)
            headers[name.lower()] = value.strip()
        size = int(headers['content-length'])
        body = self.reader.read(size)
        if len(body) != size:
            raise AssertionError('truncated body')
        return status, headers, body

    def assert_closed(self):
        try:
            assert self.reader.read(1) == b'', 'connection kept open'
        except ConnectionResetError:
            pass


def request_bytes(method, path, body=b'', extra=b''):
    return (f'{method} {path} HTTP/1.1\r\nHost: localhost\r\n'.encode('ascii')
            + f'Content-Length: {len(body)}\r\n'.encode('ascii')
            + extra + b'\r\n' + body)


class Server:
    def __init__(self, executable, port, extra=None):
        self.port = port
        self.log = tempfile.TemporaryFile()
        options = {}
        if os.name == 'nt':
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
            options.update(creationflags=subprocess.CREATE_NEW_CONSOLE
                           | subprocess.CREATE_NEW_PROCESS_GROUP, startupinfo=startup)
        else:
            options['start_new_session'] = True
        self.process = subprocess.Popen([str(Path(executable).resolve()), str(port)] + (extra or []),
                                        stdout=self.log, stderr=self.log, **options)
        try:
            until = time.monotonic() + 8
            while time.monotonic() < until:
                if self.process.poll() is not None:
                    raise AssertionError(f'server startup exit {self.process.returncode}')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.1):
                        return
                except OSError:
                    time.sleep(.02)
            raise AssertionError('server startup timeout')
        except BaseException:
            self.process.kill()
            self.process.wait()
            self.log.close()
            raise

    def stop(self):
        try:
            if self.process.poll() is None:
                if os.name == 'nt':
                    # Attach only this helper to the server's private hidden console.
                    helper = '''import ctypes, sys
k = ctypes.WinDLL('kernel32', use_last_error=True)
k.FreeConsole()
if not k.AttachConsole(int(sys.argv[1])):
    raise ctypes.WinError(ctypes.get_last_error())
k.SetConsoleCtrlHandler(None, True)
if not k.GenerateConsoleCtrlEvent(1, int(sys.argv[1])):
    raise ctypes.WinError(ctypes.get_last_error())
'''
                    subprocess.run([sys.executable, '-c', helper, str(self.process.pid)],
                                   check=True, timeout=5,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
                else:
                    os.killpg(self.process.pid, signal.SIGINT)
            code = self.process.wait(timeout=5)
            if code != 0:
                raise AssertionError(f'server shutdown exit {code}')
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=5)
            self.log.close()


class FunctionalTests(unittest.TestCase):
    port = None

    def api(self, method, path, body=None, headers=None, connection=None):
        own = connection is None
        connection = connection or http.client.HTTPConnection('127.0.0.1', self.port, timeout=4)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            data = response.read()
            self.assertEqual(int(response.getheader('Content-Length')), len(data))
            self.assertEqual(response.getheader('Cache-Control'), 'no-store')
            return response.status, data, response.getheader('Content-Type')
        finally:
            if own:
                connection.close()

    def test_api_keepalive_binary_empty(self):
        with closing(http.client.HTTPConnection('127.0.0.1', self.port, timeout=4)) as client:
            status, body, _ = self.api('GET', '/health', connection=client)
            self.assertEqual(status, 200)
            health = json.loads(body)
            self.assertIs(health['ok'], True)
            for field in ['compiler', 'flags', 'optimization', 'build_timestamp', 'source']:
                self.assertTrue(health['build_info'][field])
            for value in (b'', bytes(range(256)) * 7):
                self.assertEqual(self.api('PUT', '/keys/a%20b%252F', value, connection=client)[0], 200)
                self.assertEqual(self.api('GET', '/keys/a%20b%252F', connection=client),
                                 (200, value, 'application/octet-stream'))
            self.assertEqual(self.api('DELETE', '/keys/a%20b%252F', connection=client)[0], 200)
            self.assertEqual(json.loads(self.api('GET', '/keys/a%20b%252F', connection=client)[1]),
                             {'error': 'not_found'})

    def test_keys_beyond_initial_capacity_survive_growth(self):
        # The server starts with 1024 buckets; 1025 distinct keys force at least one growth.
        keys = [f'grow{index}' for index in range(1025)]
        with closing(http.client.HTTPConnection('127.0.0.1', self.port, timeout=4)) as client:
            for key in keys:
                self.assertEqual(self.api('PUT', f'/keys/{key}', key.encode(), connection=client)[0], 200)
            for key in keys:
                self.assertEqual(self.api('GET', f'/keys/{key}', connection=client)[:2], (200, key.encode()))
            for key in keys[::100]:
                self.assertEqual(self.api('PUT', f'/keys/{key}', b'replaced', connection=client)[0], 200)
            for key in keys[1::100]:
                self.assertEqual(self.api('DELETE', f'/keys/{key}', connection=client)[0], 200)
            for index, key in enumerate(keys):
                status, body, _ = self.api('GET', f'/keys/{key}', connection=client)
                if index % 100 == 0:
                    self.assertEqual((status, body), (200, b'replaced'))
                elif index % 100 == 1:
                    self.assertEqual((status, json.loads(body)), (404, {'error': 'not_found'}))
                else:
                    self.assertEqual((status, body), (200, key.encode()))

    def test_ttl_expired_then_absent_and_overwrite(self):
        self.assertEqual(self.api('PUT', '/keys/ttl', b'v', {'X-TTL-Ms': '40'})[0], 200)
        time.sleep(.08)
        status, body, _ = self.api('GET', '/keys/ttl')
        self.assertEqual((status, json.loads(body)), (404, {'error': 'expired'}))
        self.assertEqual(json.loads(self.api('GET', '/keys/ttl')[1]), {'error': 'not_found'})
        self.api('PUT', '/keys/ttl', b'old', {'X-TTL-Ms': '40'})
        self.api('PUT', '/keys/ttl', b'persistent')
        time.sleep(.08)
        self.assertEqual(self.api('GET', '/keys/ttl')[:2], (200, b'persistent'))

    def test_pipeline_order_internal_progress_and_eof(self):
        with RawClient(self.port) as client:
            requests = [request_bytes('PUT', '/keys/pipeline', b'first'),
                        request_bytes('GET', '/keys/pipeline'),
                        request_bytes('PUT', '/keys/pipeline', b'second'),
                        request_bytes('GET', '/keys/pipeline')]
            # More than the bounded staging area; no further client writes after this.
            requests.extend([request_bytes('GET', '/health', extra=b'X-Padding: ' + b'x' * 100 + b'\r\n')] * 450)
            client.socket.sendall(b''.join(requests))
            client.socket.shutdown(socket.SHUT_WR)
            bodies = [client.response()[2] for _ in requests]
            self.assertEqual(bodies[:4], [b'{"ok":true}', b'first', b'{"ok":true}', b'second'])
            self.assertTrue(all(json.loads(body)['ok'] is True and 'build_info' in json.loads(body)
                                for body in bodies[4:]))
            client.assert_closed()

    def test_fragmented_header_and_body(self):
        body = b'\x00\xff\r\nfragment'
        request = request_bytes('PUT', '/keys/fragment', body)
        with RawClient(self.port) as client:
            split = len(request) - len(body)
            for piece in (request[:7], request[7:split - 3], request[split - 3:split + 2], request[split + 2:]):
                client.socket.sendall(piece)
                time.sleep(.01)
            self.assertEqual(client.response()[0], 200)
            client.socket.sendall(request_bytes('GET', '/keys/fragment', extra=b'Connection: close\r\n'))
            self.assertEqual(client.response()[2], body)
            client.assert_closed()

    def test_multiple_clients_and_slow_peer(self):
        with RawClient(self.port) as slow:
            slow.socket.sendall(b'PUT /keys/slow HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\nx')
            def round_trip(index):
                value = str(index).encode()
                self.assertEqual(self.api('PUT', f'/keys/client{index}', value)[0], 200)
                self.assertEqual(self.api('GET', f'/keys/client{index}')[:2], (200, value))
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(round_trip, range(16)))
            self.assertEqual(self.api('GET', '/health')[0], 200)

    def test_large_response_snapshot_and_disconnect(self):
        original = bytes(range(256)) * 4096
        self.assertEqual(self.api('PUT', '/keys/large', original)[0], 200)
        with RawClient(self.port) as slow:
            slow.socket.sendall(request_bytes('GET', '/keys/large'))
            # Receiving the headers proves the GET executed before replacing the store value.
            status = slow.reader.readline()
            self.assertIn(b'200', status)
            headers = {}
            while True:
                line = slow.reader.readline()
                if line == b'\r\n':
                    break
                name, value = line.split(b':', 1)
                headers[name.lower()] = value.strip()
            self.api('PUT', '/keys/large', b'replaced')
            self.api('DELETE', '/keys/large')
            self.assertEqual(int(headers[b'content-length']), len(original))
            self.assertEqual(slow.reader.read(len(original)), original)
        # Reset during a pending response and disconnect during an incomplete request.
        self.api('PUT', '/keys/large', original)
        for payload in (request_bytes('GET', '/keys/large'), b'PUT /keys/broken HTTP/1.1\r\n'):
            sock = socket.create_connection(('127.0.0.1', self.port), timeout=4)
            sock.sendall(payload)
            linger_format = 'HH' if os.name == 'nt' else 'ii'
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack(linger_format, 1, 0))
            sock.close()
        self.assertEqual(self.api('GET', '/health')[0], 200)

    def test_invalid_requests_and_limits(self):
        cases = [
            (b'PUT /keys/x HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n', 400),
            (b'PUT /keys/x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n', 400),
            (request_bytes('PUT', '/keys/x', extra=b'X-TTL-Ms: 0\r\n'), 400),
            (b'PUT /keys/x HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n', 413),
            (request_bytes('GET', '/keys/' + 'k' * 1025), 414),
            (request_bytes('GET', '/health', extra=b'X-Large: ' + b'x' * 8192 + b'\r\n'), 431),
            (request_bytes('POST', '/health'), 405),
        ]
        for payload, expected in cases:
            with self.subTest(status=expected, prefix=payload[:40]), RawClient(self.port) as client:
                client.socket.sendall(payload)
                status, _, body = client.response()
                self.assertEqual(status, expected)
                self.assertIn('error', json.loads(body))
                if expected != 405:
                    client.assert_closed()

    def test_connection_limit_and_recovery(self):
        clients = []
        try:
            for _ in range(128):
                client = RawClient(self.port)
                clients.append(client)
                client.socket.sendall(request_bytes('GET', '/health'))
                self.assertEqual(client.response()[0], 200)
            with RawClient(self.port) as rejected:
                rejected.assert_closed()
            # Existing clients still progress while the extra connection is rejected.
            clients[0].socket.sendall(request_bytes('GET', '/health'))
            self.assertEqual(clients[0].response()[0], 200)
        finally:
            for client in clients:
                client.__exit__()
        self.assertEqual(self.api('GET', '/health')[0], 200)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', required=True)
    parser.add_argument('--port', type=int, default=0, help='0 chooses a free loopback port')
    parser.add_argument('--bucket-structure', choices=['list', 'rbtree'])
    args = parser.parse_args()
    if args.port == 0:
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            args.port = probe.getsockname()[1]
    server = Server(args.server, args.port, ['--bucket-structure', args.bucket_structure] if args.bucket_structure else None)
    FunctionalTests.port = args.port
    success = False
    try:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(FunctionalTests)
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        success = result.wasSuccessful()
    finally:
        # Include active idle and partial-request connections in shutdown cleanup.
        with ExitStack() as connections:
            try:
                idle = connections.enter_context(RawClient(args.port))
                partial = connections.enter_context(RawClient(args.port))
                partial.socket.sendall(b'GET /hea')
            finally:
                server.stop()
            idle.assert_closed()
            partial.assert_closed()
        assert_port_released(args.port)
        print('Shutdown: exit 0; active sockets closed; listening port released')
    return 0 if success else 1


if __name__ == '__main__':
    sys.exit(main())

