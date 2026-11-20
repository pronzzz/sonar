#!/usr/bin/env python3
"""A stand-in for the part of GitHub that Sonar's updater uses, for tests
and for updating devices from a local machine (docs/updates.md).

    update_server.py ROOT [--port 8443] [--tls CERT KEY] [--public-url URL]

ROOT/OWNER/REPO/ holds one release: its files (sonar-update.txt and the
kernel image), plus optional `tag` (the tag name, default v<version>) and
`token` (if present, requests must carry "Authorization: Bearer <token>",
otherwise they get 404 like a private GitHub repository).

Endpoints, shaped like GitHub's:
  GET /repos/OWNER/REPO/releases/latest        release JSON with assets
  GET /repos/OWNER/REPO/releases/assets/N      302 to /download/... (Accept:
                                               application/octet-stream), as
                                               GitHub redirects to its CDN
  GET /download/OWNER/REPO/NAME                the file; refuses requests that
                                               carry an Authorization header
                                               (a client must not leak its
                                               token to the download host)
  GET /mirror/OWNER/REPO/NAME                  the file (for update.mirror)

With --tls it speaks HTTPS (TLS 1.3 needs Python built with OpenSSL 1.1.1+).
--public-url is the base URL clients use (default https://10.0.2.2:PORT, the
host as a QEMU guest with user networking sees it).
"""
import argparse
import http.server
import json
import os
import ssl
import sys
from urllib.parse import urlsplit

ARGS = None


def release_dir(owner, repo):
    d = os.path.join(ARGS.root, owner, repo)
    return d if os.path.isdir(d) else None


def assets(d):
    names = sorted(n for n in os.listdir(d) if n not in ("tag", "token") and not n.startswith("."))
    return names


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        sys.stderr.write("update_server: %s %s\n" % (self.address_string(), fmt % a))

    def send(self, code, body=b"", ctype="application/json", headers=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def not_found(self):
        self.send(404, json.dumps({"message": "Not Found",
                                   "documentation_url": "https://docs.github.com/rest"}).encode())

    def authorized(self, d):
        tf = os.path.join(d, "token")
        if not os.path.exists(tf):
            return True
        want = "Bearer " + open(tf).read().strip()
        return self.headers.get("Authorization", "") == want

    def do_GET(self):
        path = urlsplit(self.path).path
        parts = [p for p in path.split("/") if p]
        base = ARGS.public_url.rstrip("/")
        if len(parts) == 5 and parts[0] == "repos" and parts[3:] == ["releases", "latest"]:
            d = release_dir(parts[1], parts[2])
            if not d or not self.authorized(d):
                return self.not_found()
            names = assets(d)
            tag = open(os.path.join(d, "tag")).read().strip() if os.path.exists(
                os.path.join(d, "tag")) else "v-test"
            rel = {
                "tag_name": tag, "name": "Sonar " + tag, "draft": False, "prerelease": False,
                "body": "Test release served by tools/update_server.py.\n" * 20,
                "assets": [{
                    "url": "%s/repos/%s/%s/releases/assets/%d" % (base, parts[1], parts[2], i + 1),
                    "id": i + 1, "name": n, "size": os.path.getsize(os.path.join(d, n)),
                    "content_type": "application/octet-stream",
                    "browser_download_url": "%s/download/%s/%s/%s" % (base, parts[1], parts[2], n),
                } for i, n in enumerate(names)],
            }
            return self.send(200, json.dumps(rel, indent=1).encode())
        if len(parts) == 6 and parts[0] == "repos" and parts[3:5] == ["releases", "assets"]:
            d = release_dir(parts[1], parts[2])
            if not d or not self.authorized(d):
                return self.not_found()
            names = assets(d)
            i = int(parts[5]) - 1 if parts[5].isdigit() else -1
            if not 0 <= i < len(names):
                return self.not_found()
            if self.headers.get("Accept") != "application/octet-stream":
                return self.send(200, json.dumps({"name": names[i]}).encode())
            loc = "%s/download/%s/%s/%s?signature=test" % (base, parts[1], parts[2], names[i])
            return self.send(302, b"", "text/plain", [("Location", loc)])
        if len(parts) == 4 and parts[0] in ("download", "mirror"):
            if parts[0] == "download" and self.headers.get("Authorization"):
                return self.send(400, b"Authorization header sent to the download host\n",
                                 "text/plain")
            d = release_dir(parts[1], parts[2])
            f = os.path.join(d, parts[3]) if d else None
            if not f or parts[3] in ("tag", "token") or not os.path.isfile(f):
                return self.not_found()
            if parts[0] == "mirror" and not self.authorized(d):
                return self.not_found()
            return self.send(200, open(f, "rb").read(), "application/octet-stream")
        return self.not_found()


def main():
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--tls", nargs=2, metavar=("CERT", "KEY"))
    ap.add_argument("--public-url")
    ARGS = ap.parse_args()
    if not ARGS.public_url:
        ARGS.public_url = "%s://10.0.2.2:%d" % ("https" if ARGS.tls else "http", ARGS.port)
    srv = http.server.ThreadingHTTPServer((ARGS.bind, ARGS.port), Handler)
    if ARGS.tls:
        if not ssl.HAS_TLSv1_3:
            sys.exit("this Python's ssl module has no TLS 1.3 (%s)" % ssl.OPENSSL_VERSION)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        ctx.load_cert_chain(*ARGS.tls)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print("update_server: serving %s on %s:%d as %s" % (ARGS.root, ARGS.bind, ARGS.port,
                                                         ARGS.public_url), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
