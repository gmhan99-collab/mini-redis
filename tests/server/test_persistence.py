"""Run against the isolated server built by make persistence-test."""

import argparse
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class PersistenceTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="mini-redis-persistence-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.clock = self.root / "monotonic-ms"
        self.set_clock(10_000)
        self.path = self.root / "new store"
        self.process = None

    def set_clock(self, value):
        self.clock.write_text(str(value), encoding="ascii")

    def args(self, mode, action, structure, path, port):
        args = [self.executable, str(port), "--bucket-structure", structure,
                "--buckets", "1", "--persistence", mode]
        if action is not None:
            args += ["--data-action", action, "--data-dir", str(path)]
        return args

    def start(self, mode, action, structure="list", failure=None, signal_group=False):
        self.port = free_port()
        env = os.environ.copy()
        env["MINI_REDIS_TEST_CLOCK_PATH"] = str(self.clock)
        env.pop("MINI_REDIS_TEST_FAIL_SYNC", None)
        if failure:
            env["MINI_REDIS_TEST_FAIL_SYNC"] = failure
        flags = 0
        if signal_group and os.name == "nt":
            flags = subprocess.CREATE_NEW_PROCESS_GROUP
        process = subprocess.Popen(
            self.args(mode, action, structure, self.path, self.port),
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=flags,
        )
        self.process = process
        self.addCleanup(self.clean_child, process)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if process.poll() is not None:
                self.fail(process.stdout.read().decode(errors="replace"))
            try:
                status, headers, data = self.request("GET", "/health")
                self.assertEqual(status, 200)
                self.assertEqual(json.loads(data)["persistence"]["mode"], mode)
                self.assertIn("build_info", json.loads(data))
                return
            except ConnectionRefusedError:
                time.sleep(0.03)
        self.fail("server readiness deadline expired")

    @staticmethod
    def clean_child(process):
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        process.stdout.close()

    def rejected_start(self, mode, action, path=None, port=None):
        result = subprocess.run(
            self.args(mode, action, "list", self.path if path is None else path, port or free_port()),
            capture_output=True, timeout=6, check=False,
        )
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertNotIn(b"listening", result.stdout)
        return result.stderr

    def request(self, method, path, body=None, ttl=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=6)
        headers = {} if ttl is None else {"X-TTL-Ms": str(ttl)}
        try:
            conn.request(method, path, body=body, headers=headers)
            response = conn.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            conn.close()

    def put(self, key, value, ttl=None):
        status, headers, data = self.request("PUT", "/keys/" + key, value, ttl)
        self.assertEqual(status, 200, data)
        self.assertRegex(headers["Server-Timing"], r"^engine;dur=\d+\.\d+$")

    def get(self, key, expected):
        status, _, data = self.request("GET", "/keys/" + key)
        if expected is None:
            self.assertEqual(status, 404, data)
        else:
            self.assertEqual(status, 200, data[:64])
            self.assertEqual(data, expected)

    def control(self, suffix="", method="GET", expected=200):
        status, _, data = self.request(method, "/persistence" + suffix)
        self.assertEqual(status, expected, data)
        return json.loads(data)

    def save(self):
        result = self.control("/save", "POST")
        self.assertTrue(result["ok"])
        self.assertGreaterEqual(result["save_ms"], 0)
        self.assertEqual(result["persistence"]["data_bytes"], (self.path / "snapshot.rdb").stat().st_size)

    def stop(self):
        result = self.control("/shutdown", "POST")
        self.assertTrue(result["ok"])
        self.assertGreaterEqual(result["checkpoint_ms"], 0)
        self.assertEqual(self.process.wait(timeout=5), 0)
        return result["persistence"]

    def test_none_and_control_validation(self):
        self.start("none", None)
        status = self.control()["persistence"]
        self.assertEqual(status["sync_policy"], "none")
        self.assertIsNone(status["format_version"])
        self.assertEqual(status["data_bytes"], 0)
        self.assertEqual(self.control("/save", "POST", 409)["error"], "not_rdb_mode")
        self.assertEqual(self.control("/shutdown", "GET", 405)["error"], "method_not_allowed")
        self.assertEqual(self.control("?debug=yes", expected=400)["error"], "query_not_supported")
        status, _, body = self.request("POST", "/persistence/shutdown", b"x")
        self.assertEqual((status, json.loads(body)["error"]), (400, "body_not_allowed"))
        self.put("lost", b"transient")
        self.stop()
        self.start("none", None)
        self.get("lost", None)
        self.stop()
        self.assertFalse(self.path.exists())
        self.rejected_start("none", "create")
        self.rejected_start("rdb", None)

    def test_rdb_unpublished_data_and_explicit_open(self):
        self.rejected_start("rdb", "open")
        self.rejected_start("aof", "create", self.root / "missing-parent" / "store")
        self.start("rdb", "create")
        self.put("lost", b"unsaved")
        self.rejected_start("rdb", "create")
        self.rejected_start("rdb", "open")
        self.assertEqual(self.stop()["data_bytes"], 0)
        self.assertFalse((self.path / "snapshot.rdb").exists())
        self.rejected_start("aof", "open")
        self.start("rdb", "open")
        self.get("lost", None)
        self.stop()

    def test_rdb_snapshot_mutations_ttl_and_bucket_structures(self):
        for structure in ("list", "rbtree"):
            with self.subTest(structure=structure):
                self.path = self.root / structure
                self.set_clock(10_000)
                self.start("rdb", "create", structure)
                self.put("bytes%0A%FF", b"\x00\n\r\xff")
                self.put("empty", b"")
                self.put("changed", b"old", 100)
                self.put("deleted", b"old")
                self.put("expired_at_save", b"old", 8)
                self.assertEqual(self.request("DELETE", "/keys/deleted")[0], 200)
                self.set_clock(10_010)
                self.save()
                snapshot = (self.path / "snapshot.rdb").read_bytes()
                self.put("changed", b"unsaved new", 10_000)
                self.put("unsaved", b"new")
                self.set_clock(10_060)
                self.assertEqual(self.stop()["logical_elapsed_ms"], 60)
                self.assertEqual((self.path / "snapshot.rdb").read_bytes(), snapshot)
                time.sleep(0.12)  # Outside the process, longer than the 40 ms remaining TTL.
                self.set_clock(300_000_000)
                self.start("rdb", "open", structure)
                status = self.control()["persistence"]
                self.assertEqual(status["sync_policy"], "manual_snapshot")
                self.assertEqual(status["format_version"], 1)
                self.assertEqual(status["logical_elapsed_ms"], 60)
                self.assertGreaterEqual(status["restore_ms"], 0)
                self.get("bytes%0A%FF", b"\x00\n\r\xff")
                self.get("empty", b"")
                for missing in ("deleted", "unsaved", "expired_at_save"):
                    self.get(missing, None)
                self.set_clock(300_000_039)
                self.get("changed", b"old")
                self.stop()
                self.set_clock(100)
                other = "list" if structure == "rbtree" else "rbtree"
                self.start("rdb", "open", other)
                self.get("changed", b"old")
                self.set_clock(101)
                self.get("changed", None)
                self.stop()

    def test_aof_replays_mutations_and_expiration_in_both_structures(self):
        for structure in ("list", "rbtree"):
            with self.subTest(structure=structure):
                self.path = self.root / structure
                self.set_clock(10_000)
                self.start("aof", "create", structure)
                aof = self.path / "append.aof"
                initial_length = aof.stat().st_size
                self.assertEqual(self.control("/save", "POST", 409)["error"], "not_rdb_mode")
                self.put("bytes%0A%FF", b"\x00\n\xff")
                self.put("empty", b"")
                self.put("gone", b"old")
                self.assertEqual(self.request("DELETE", "/keys/gone")[0], 200)
                self.put("overwritten", b"no expiration")
                self.put("overwritten", b"expires", 30)
                self.put("remove-ttl", b"old", 20)
                self.put("remove-ttl", b"new")
                self.put("reset-ttl", b"old", 10)
                self.set_clock(10_005)
                self.put("reset-ttl", b"new", 95)
                self.put("maximum", bytes(range(256)) * 4096)
                self.set_clock(10_050)
                status = self.control()["persistence"]
                self.assertEqual(status["sync_policy"], "per_mutation")
                self.assertGreater(status["data_bytes"], initial_length)
                self.assertEqual(status["data_bytes"], aof.stat().st_size)
                self.assertEqual(self.request("DELETE", "/keys/overwritten")[0], 404)
                self.assertEqual(aof.stat().st_size, status["data_bytes"])
                self.stop()
                time.sleep(0.12)
                self.set_clock(100)
                self.start("aof", "open", structure)
                self.assertEqual(self.control()["persistence"]["logical_elapsed_ms"], 50)
                self.get("bytes%0A%FF", b"\x00\n\xff")
                self.get("empty", b"")
                self.get("maximum", bytes(range(256)) * 4096)
                self.get("gone", None)
                self.assertEqual(json.loads(self.request("GET", "/keys/overwritten")[2])["error"], "not_found")
                self.get("remove-ttl", b"new")
                self.get("reset-ttl", b"new")
                self.set_clock(135)
                self.stop()
                self.set_clock(2_000_000_000)
                self.start("aof", "open", structure)
                self.set_clock(2_000_000_014)
                self.get("reset-ttl", b"new")
                self.set_clock(2_000_000_015)
                self.get("reset-ttl", None)
                self.stop()
                self.assertEqual(aof.stat().st_size, status["data_bytes"])

    def test_aof_replays_after_clock_moves_backward(self):
        self.start("aof", "create")
        self.set_clock(14_000)
        self.put("first", b"record")
        self.set_clock(15_000)
        self.assertEqual(self.control()["persistence"]["logical_elapsed_ms"], 5_000)
        self.set_clock(12_000)
        self.put("second", b"record", ttl=1_000)
        stopped = self.stop()

        self.set_clock(20)
        self.start("aof", "open")
        self.assertEqual(stopped["logical_elapsed_ms"], 5_000)
        self.assertEqual(self.control()["persistence"]["logical_elapsed_ms"], 5_000)
        self.get("first", b"record")
        self.set_clock(1_019)
        self.get("second", b"record")
        self.set_clock(1_020)
        self.get("second", None)
        self.stop()

    def test_rdb_does_not_restore_expiry_observed_before_clock_moves_backward(self):
        self.start("rdb", "create")
        self.put("expires", b"old", ttl=1_000)
        self.put("kept", b"value")
        self.save()
        self.set_clock(11_000)
        self.get("expires", None)
        self.set_clock(9_000)
        self.stop()

        self.set_clock(20)
        self.start("rdb", "open")
        self.get("expires", None)
        self.get("kept", b"value")
        self.stop()

    def test_stale_checkpoint_temp_is_reported_and_preserved(self):
        self.start("aof", "create")
        self.put("key", b"retained")
        self.stop()
        checkpoint = self.path / "clock"
        original = checkpoint.read_bytes()
        temp = self.path / "clock.tmp"
        temp.write_bytes(b"existing temporary filename")
        stderr = self.rejected_start("aof", "open")
        self.assertIn(f"{self.path}/clock.tmp".encode(), stderr)
        self.assertIn(b"already exists", stderr)
        self.assertEqual(temp.read_bytes(), b"existing temporary filename")
        self.assertEqual(checkpoint.read_bytes(), original)
        temp.unlink()
        self.start("aof", "open")
        self.get("key", b"retained")
        self.stop()

    def check_rdb_file_collision(self, suffix):
        self.start("rdb", "create")
        self.put("key", b"old")
        self.save()
        snapshot = self.path / "snapshot.rdb"
        old = snapshot.read_bytes()
        self.put("key", b"new")
        collision = self.path / ("snapshot.rdb" + suffix)
        collision.write_bytes(b"existing private filename")
        self.assertEqual(self.control("/save", "POST", 500)["error"], "persistence_error")
        self.assertEqual(collision.read_bytes(), b"existing private filename")
        self.assertEqual(snapshot.read_bytes(), old)
        self.get("key", b"new")
        collision.unlink()
        self.save()
        self.stop()
        output = self.process.stdout.read()
        self.assertIn(f"{self.path}/snapshot.rdb{suffix}".encode(), output)
        self.assertIn(b"already exists", output)
        self.start("rdb", "open")
        self.get("key", b"new")
        self.stop()

    def test_rdb_file_error_preserves_previous_snapshot(self):
        self.check_rdb_file_collision(".tmp")

    @unittest.skipIf(os.name == "nt", "POSIX replacement backup")
    def test_rdb_stale_backup_is_reported_and_preserved(self):
        self.check_rdb_file_collision(".previous")

    def check_rdb_sync_failure(self, failure):
        self.start("rdb", "create")
        self.put("key", b"old")
        self.save()
        self.stop()
        snapshot = self.path / "snapshot.rdb"
        original = snapshot.read_bytes()
        self.start("rdb", "open", failure=failure)
        self.put("key", b"new")
        self.assertEqual(self.control("/save", "POST", 500)["error"], "persistence_error")
        self.assertEqual(snapshot.read_bytes(), original)
        self.get("key", b"new")
        self.stop()
        self.start("rdb", "open")
        self.get("key", b"old")
        self.stop()

    def test_rdb_sync_error_keeps_previous_snapshot_and_running_store(self):
        self.check_rdb_sync_failure("snapshot")

    @unittest.skipIf(os.name == "nt", "POSIX directory fsync rollback")
    def test_rdb_directory_sync_error_rolls_back_publication(self):
        self.check_rdb_sync_failure("snapshot-directory")

    def test_aof_sync_error_replies_then_exits_dirty(self):
        self.start("aof", "create", failure="append")
        status, _, data = self.request("PUT", "/keys/failed", b"new")
        self.assertEqual((status, json.loads(data)["error"]), (500, "persistence_error"))
        self.assertNotEqual(self.process.wait(timeout=5), 0)
        self.rejected_start("aof", "open")

    def test_checkpoint_sync_error_replies_then_exits_dirty(self):
        for mode in ("aof", "rdb"):
            with self.subTest(mode=mode):
                self.path = self.root / mode
                self.start(mode, "create", failure="checkpoint")
                self.put("key", b"new")
                self.assertEqual(self.control("/shutdown", "POST", 500)["error"], "persistence_error")
                self.assertNotEqual(self.process.wait(timeout=5), 0)
                self.rejected_start(mode, "open")

    def test_missing_and_mismatched_checkpoint_is_rejected(self):
        self.start("rdb", "create")
        self.put("key", b"value")
        self.save()
        self.stop()
        clock = self.path / "clock"
        record = clock.read_bytes()
        clock.unlink()
        self.rejected_start("rdb", "open")
        clock.write_bytes(record[:22] + (2).to_bytes(8, "little") + record[30:])
        self.rejected_start("rdb", "open")
        clock.write_bytes(record[:4] + bytes([record[4] ^ 255]) + record[5:])
        self.rejected_start("rdb", "open")
        clock.write_bytes(record)
        snapshot = self.path / "snapshot.rdb"
        data = snapshot.read_bytes()
        snapshot.write_bytes(b"MRD2" + data[4:])
        self.rejected_start("rdb", "open")
        snapshot.write_bytes(data)
        self.start("rdb", "open")
        self.get("key", b"value")
        self.stop()

    def test_shutdown_stops_before_pipelined_mutation(self):
        self.start("aof", "create")
        self.put("first", b"saved")
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as client:
            client.sendall(
                b"POST /persistence/shutdown HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"
                b"PUT /keys/pipelined HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx"
            )
            with client.makefile("rb") as received:
                data = received.read()
            self.assertEqual(data.count(b"HTTP/1.1 "), 1)
            self.assertIn(b"HTTP/1.1 200", data)
        self.assertEqual(self.process.wait(timeout=5), 0)
        self.start("aof", "open")
        self.get("first", b"saved")
        self.get("pipelined", None)
        self.stop()

    def test_bind_failure_preserves_clean_existing_store(self):
        self.start("aof", "create")
        self.put("key", b"retained")
        self.stop()
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            sock.listen()
            self.rejected_start("aof", "open", port=sock.getsockname()[1])
        self.start("aof", "open")
        self.get("key", b"retained")
        self.stop()

    def test_console_signal_checkpoints_before_exit(self):
        self.start("aof", "create", signal_group=True)
        self.put("key", b"retained")
        self.set_clock(10_080)
        shutdown_signal = signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGTERM
        try:
            self.process.send_signal(shutdown_signal)
        except OSError as error:
            if os.name == "nt" and error.winerror == 6:
                self.skipTest("the test launcher has no Windows console")
            raise
        self.assertEqual(self.process.wait(timeout=5), 0)
        self.set_clock(10)
        self.start("aof", "open")
        self.assertEqual(self.control()["persistence"]["logical_elapsed_ms"], 80)
        self.get("key", b"retained")
        self.stop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    options, remaining = parser.parse_known_args()
    PersistenceTest.executable = str(Path(options.server).resolve())
    unittest.main(argv=[__file__, *remaining], verbosity=2)
