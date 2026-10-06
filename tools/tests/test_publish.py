"""make check-publish: runs tools/publish-repo.py against a local server
that behaves as the generic package registry of Forgejo. The server
refuses requests without the token, refuses to replace a file, lists
the files of a version with their SHA-256 digests, and records every
change in the order of arrival."""
import hashlib
import http.server
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(TOP, "tools", "publish-repo.py")
AUTH = "Basic dGVzdGVyOnNlY3JldA=="     # tester:secret


class Registry(http.server.BaseHTTPRequestHandler):
    files = {}                          # (package, version, name) -> bytes
    log = []

    def log_message(self, *args):
        pass

    def reply(self, status, body=b""):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def target(self):
        # /api/packages/tester/generic/PACKAGE/VERSION/NAME
        parts = self.path.strip("/").split("/")
        if len(parts) == 7 and parts[:4] == ["api", "packages", "tester", "generic"]:
            return tuple(parts[4:])
        return None

    def authorized(self):
        if self.headers.get("Authorization") != AUTH:
            self.reply(401, b"unauthorized")
            return False
        return True

    def do_GET(self):
        parts = self.path.strip("/").split("/")
        if len(parts) == 8 and parts[:5] == ["api", "v1", "packages", "tester", "generic"] and parts[7] == "files":
            listing = [{"name": k[2], "Size": len(v), "sha256": hashlib.sha256(v).hexdigest()}
                       for k, v in self.files.items() if k[:2] == (parts[5], parts[6])]
            self.reply(200 if listing else 404, json.dumps(listing).encode())
            return
        key = self.target()
        if key in self.files:
            self.reply(200, self.files[key])
        else:
            self.reply(404)

    def do_PUT(self):
        key = self.target()
        if not self.authorized():
            return
        data = self.rfile.read(int(self.headers["Content-Length"]))
        if key is None:
            self.reply(400)
        elif key in self.files:
            self.reply(409, b"file exists")
        else:
            self.files[key] = data
            self.log.append(("PUT", key[2]))
            self.reply(201)

    def do_DELETE(self):
        key = self.target()
        if not self.authorized():
            return
        if key not in self.files:
            self.reply(404)
        else:
            del self.files[key]
            self.log.append(("DELETE", key[2]))
            self.reply(204)


class PublishTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Registry)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}/api/packages/tester/generic"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def setUp(self):
        Registry.files.clear()
        Registry.log.clear()
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, files):
        for name in os.listdir(self.repo):
            os.unlink(os.path.join(self.repo, name))
        for name, data in files.items():
            with open(os.path.join(self.repo, name), "wb") as f:
                f.write(data)

    def publish(self, token="secret"):
        env = dict(os.environ, PUBLISH_TOKEN=token, PUBLISH_USER="tester")
        return subprocess.run([sys.executable, SCRIPT, self.base, self.repo, "x86_64"], env=env,
                              capture_output=True, text=True)

    def remote(self):
        return {k[2]: v for k, v in Registry.files.items() if k[:2] == ("minios-x86_64", "repo")}

    def test_publish_sequence(self):
        first = {"a-1.mpk": b"a1", "b-1.mpk": b"b1", "index": b"index 1", "index.sig": b"sig 1"}
        self.write(first)
        r = self.publish()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.remote(), first)
        # The archives arrive before the index, and the signature last.
        names = [n for _, n in Registry.log]
        self.assertEqual(names[-2:], ["index", "index.sig"])
        self.assertIn("2 uploaded, 0 unchanged, 0 removed, index replaced", r.stdout)

        Registry.log.clear()
        r = self.publish()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(Registry.log, [])
        self.assertIn("0 uploaded, 2 unchanged, 0 removed, index unchanged", r.stdout)

        # b changes, a goes, c arrives. The removal of a follows the new
        # index, and the changed b is deleted before its upload.
        second = {"b-1.mpk": b"b1 rebuilt", "c-1.mpk": b"c1", "index": b"index 2", "index.sig": b"sig 2"}
        self.write(second)
        Registry.log.clear()
        r = self.publish()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.remote(), second)
        self.assertEqual(Registry.log, [("DELETE", "b-1.mpk"), ("PUT", "b-1.mpk"), ("PUT", "c-1.mpk"),
                                        ("DELETE", "index"), ("DELETE", "index.sig"), ("PUT", "index"),
                                        ("PUT", "index.sig"), ("DELETE", "a-1.mpk")])

    def test_token(self):
        self.write({"a-1.mpk": b"a1", "index": b"i", "index.sig": b"s"})
        r = self.publish(token="")
        self.assertEqual(r.returncode, 2)
        self.assertIn("PUBLISH_TOKEN", r.stderr)
        r = self.publish(token="wrong")
        self.assertEqual(r.returncode, 1)
        self.assertIn("status 401", r.stderr)
        self.assertEqual(self.remote(), {})

    def test_missing_index(self):
        self.write({"a-1.mpk": b"a1"})
        r = self.publish()
        self.assertEqual(r.returncode, 1)
        self.assertIn("run make repo", r.stderr)


if __name__ == "__main__":
    unittest.main()
