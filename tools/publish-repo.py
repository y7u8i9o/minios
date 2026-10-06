#!/usr/bin/env python3
"""Upload a package repository to the generic package registry of Forgejo
(docs/design/packages.md, docs/plan/tls.md T5).

    tools/publish-repo.py BASE DIR ARCH

BASE is the URL of the generic registry of the owner, for example
https://code.calcraft.org/api/packages/flifez/generic. DIR is the
repository that `make repo` writes to build/repo/ARCH. The files go to
the package minios-ARCH with the version "repo", so pkg reads the
repository from BASE/minios-ARCH/repo. The environment variable
PUBLISH_TOKEN contains an access token with the scope write:package.
PUBLISH_USER contains the user of the token and is the owner of BASE by
default.

The registry does not replace a file. The script therefore uploads the
new and changed archives first and deletes a changed archive before its
upload. The script then replaces index and index.sig, and at the end
deletes the archives that the new index does not list. A client that
reads the repository during the run sees the old index with its
archives or the new index with its archives. The exception is the
moment between the deletion and the upload of the index files, in which
pkg reports a failed download or a wrong signature."""
import base64
import hashlib
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

VERSION = "repo"


def fail(message, status=1):
    print(f"publish-repo: {message}", file=sys.stderr)
    sys.exit(status)


def main():
    if len(sys.argv) != 4:
        fail("usage: publish-repo.py BASE DIR ARCH", 2)
    base, directory, arch = sys.argv[1].rstrip("/"), sys.argv[2], sys.argv[3]
    parts = urllib.parse.urlsplit(base)
    path = parts.path.strip("/").split("/")
    if len(path) != 4 or path[:2] != ["api", "packages"] or path[3] != "generic":
        fail(f"{base} is not the URL of a generic registry (.../api/packages/OWNER/generic)", 2)
    owner = path[2]
    token = os.environ.get("PUBLISH_TOKEN", "")
    if not token:
        fail("set PUBLISH_TOKEN to an access token with the scope write:package", 2)
    user = os.environ.get("PUBLISH_USER", owner)
    auth = "Basic " + base64.b64encode(f"{user}:{token}".encode()).decode()
    package = f"minios-{arch}"
    files_url = f"{parts.scheme}://{parts.netloc}/api/v1/packages/{owner}/generic/{package}/{VERSION}/files"
    file_url = f"{base}/{package}/{VERSION}"

    def request(method, url, data=None):
        # urllib sends a body as a form without an explicit type. Forgejo
        # refuses a form upload, so every body is a byte stream.
        headers = {"Authorization": auth}
        if data is not None:
            headers["Content-Type"] = "application/octet-stream"
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()
        except urllib.error.URLError as e:
            fail(f"{method} {url}: {e.reason}")

    def expect(method, name, data, want):
        status, body = request(method, f"{file_url}/{name}", data)
        if status != want:
            fail(f"{method} {package}/{VERSION}/{name}: status {status} {body[:200].decode(errors='replace')}")

    local = {}
    for name in sorted(os.listdir(directory)):
        with open(os.path.join(directory, name), "rb") as f:
            local[name] = f.read()
    if "index" not in local or "index.sig" not in local:
        fail(f"{directory} contains no index and index.sig; run make repo")

    status, body = request("GET", files_url)
    if status == 404:
        remote = {}
    elif status == 200:
        remote = {f["name"]: f.get("sha256", "") for f in json.loads(body)}
    else:
        fail(f"listing {package}/{VERSION}: status {status} {body[:200].decode(errors='replace')}")

    def same(name):
        return remote.get(name) == hashlib.sha256(local[name]).hexdigest()

    uploaded = unchanged = removed = 0
    archives = [n for n in local if n not in ("index", "index.sig")]
    for name in archives:
        if same(name):
            unchanged += 1
            continue
        if name in remote:
            expect("DELETE", name, None, 204)
        expect("PUT", name, local[name], 201)
        uploaded += 1
    index_changed = not (same("index") and same("index.sig"))
    if index_changed:
        for name in ("index", "index.sig"):
            if name in remote:
                expect("DELETE", name, None, 204)
        expect("PUT", "index", local["index"], 201)
        expect("PUT", "index.sig", local["index.sig"], 201)
    for name in sorted(remote):
        if name not in local:
            expect("DELETE", name, None, 204)
            removed += 1
    print(f"{package}: {uploaded} uploaded, {unchanged} unchanged, {removed} removed, "
          f"index {'replaced' if index_changed else 'unchanged'}; {file_url}")


main()
