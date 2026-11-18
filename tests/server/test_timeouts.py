"""Short-timeout real-server checks.

The server must be a test-only build with shortened limits, for example:
  gcc ... -DTCP_RECEIVE_TIMEOUT_MS=700u -DTCP_IDLE_TIMEOUT_MS=1200u -DTCP_SEND_TIMEOUT_MS=700u
Usage: python tests/server/test_timeouts.py --server build/review-tcp/mini-redis-short.exe
       [--receive-ms 700 --idle-ms 1200]
Send timeout is not exercised here: a blocked send is not guaranteed by the OS.
"""
import argparse
from pathlib import Path
import select
import socket
import sys
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_functional import RawClient, Server, request_bytes  # noqa: E402


def closed_now(sock, wait_s):
    readable, _, _ = select.select([sock], [], [], wait_s)
    if not readable:
        return False
    try:
        return sock.recv(4096) == b''
    except (ConnectionResetError, ConnectionAbortedError):
        return True


def wait_for_close(sock, limit_s):
    started = time.monotonic()
    while time.monotonic() - started < limit_s:
        if closed_now(sock, .05):
            return time.monotonic() - started
    raise AssertionError('connection not closed')


class TimeoutTests(unittest.TestCase):
    port = None
    receive_s = None
    idle_s = None
    # Generous slack for GetTickCount64 granularity and scheduler jitter.
    slack_s = .45

    def assert_health(self):
        with RawClient(self.port) as client:
            client.socket.sendall(request_bytes('GET', '/health', extra=b'Connection: close\r\n'))
            self.assertEqual(client.response()[0], 200)

    def test_idle_timeout_without_bytes(self):
        with RawClient(self.port) as client:
            elapsed = wait_for_close(client.socket, self.idle_s + 2)
        self.assertGreaterEqual(elapsed, self.idle_s - .1)
        self.assertLess(elapsed, self.idle_s + self.slack_s)
        self.assert_health()

    def test_idle_timeout_after_response(self):
        with RawClient(self.port) as client:
            client.socket.sendall(request_bytes('GET', '/health'))
            self.assertEqual(client.response()[0], 200)
            started = time.monotonic()
            self.assertFalse(closed_now(client.socket, self.idle_s * .6))
            wait_for_close(client.socket, self.idle_s + 2)
            total = time.monotonic() - started
        self.assertGreaterEqual(total, self.idle_s - .1)
        self.assertLess(total, self.idle_s + self.slack_s)

    def test_receive_timeout_is_absolute_under_trickle(self):
        request = b'PUT /keys/trickle HTTP/1.1\r\nHost: x\r\nContent-Length: 1000\r\n\r\n'
        with RawClient(self.port) as client:
            started = time.monotonic()
            client.socket.sendall(request[:5])
            offset = 5
            closed_at = None
            # Keep sending well past both limits; progress must not extend the deadline.
            while time.monotonic() - started < self.idle_s + 2:
                if closed_now(client.socket, .1):
                    closed_at = time.monotonic() - started
                    break
                try:
                    client.socket.sendall(request[offset:offset + 1] or b'x')
                except OSError:
                    closed_at = time.monotonic() - started
                    break
                offset += 1
            self.assertIsNotNone(closed_at, 'trickling connection never closed')
        self.assertGreaterEqual(closed_at, self.receive_s - .1)
        self.assertLess(closed_at, self.receive_s + self.slack_s)
        self.assert_health()

    def test_other_clients_progress_while_stalled_peer_waits(self):
        with RawClient(self.port) as stalled:
            stalled.socket.sendall(b'GET /hea')
            for _ in range(5):
                self.assert_health()
            elapsed = wait_for_close(stalled.socket, self.idle_s + 2)
        self.assertLess(elapsed, self.receive_s + self.slack_s)

    def test_listening_port_is_exclusive(self):
        probe = socket.socket()
        try:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            with self.assertRaises(OSError):
                probe.bind(('127.0.0.1', self.port))
        finally:
            probe.close()
        self.assert_health()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', required=True)
    parser.add_argument('--port', type=int, default=0)
    parser.add_argument('--receive-ms', type=int, default=700)
    parser.add_argument('--idle-ms', type=int, default=1200)
    args = parser.parse_args()
    if args.port == 0:
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            args.port = probe.getsockname()[1]
    TimeoutTests.port = args.port
    TimeoutTests.receive_s = args.receive_ms / 1000
    TimeoutTests.idle_s = args.idle_ms / 1000
    server = Server(args.server, args.port)
    try:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(TimeoutTests)
        result = unittest.TextTestRunner(verbosity=2).run(suite)
    finally:
        server.stop()
    with socket.socket() as rebound:
        rebound.bind(('127.0.0.1', args.port))
    print('Shutdown: exit 0; listening port released')
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(main())
