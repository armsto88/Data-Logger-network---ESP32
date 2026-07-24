#!/usr/bin/env python3
"""FieldMesh cloud-OTA BENCH mock — serves the whole "backend" the firmware needs.

One HTTPS host that plays both roles the real backend will later fill:

  1. The check-in endpoint. The mothership POSTs its status here; this replies
     with a command envelope carrying a single DEPLOY_RELEASE command until the
     mothership's cursor passes it (then empty envelopes).

  2. The release host. Serves the three derived artifact paths the firmware
     fetches (see otaBuildManifestUrl/otaBuildImageUrl) at the Supabase Storage
     public-read URL layout:
        GET /storage/v1/object/public/releases/<role>/<releaseId>/manifest.json
        GET /storage/v1/object/public/releases/<role>/<releaseId>/manifest.json.sig
        GET /storage/v1/object/public/releases/<role>/<releaseId>/image.bin

This is a THROWAWAY test harness — no auth, no persistence, no durability. It
exists only to bench-prove the firmware side before the real backend/dashboard
exist. Do not use it for anything real.

Run it, expose it with a public HTTPS tunnel (ngrok / cloudflared), and point a
bench firmware build at the tunnel host. See
docs/FIELDMESH_CLOUD_OTA_BENCH_RUNBOOK.md for the full procedure.

Usage:
  python scripts/mock_ota_server.py \
      --release-dir release \
      --release-id fieldmesh-2026.08.0 \
      --role mothership \
      --ingest-path /ingest \
      --port 8443 [--certfile cert.pem --keyfile key.pem]

If no cert is given, a self-signed one is generated in memory (fine — the modem
does not verify server certs; a public tunnel terminates TLS with its own cert
anyway).
"""
import argparse
import datetime
import http.server
import json
import os
import ssl
import sys
import threading

STATE = {
    "served_command": False,   # becomes True once the mothership acked (cursor advanced)
    "poll_count": 0,
}
LOCK = threading.Lock()


def make_self_signed_cert():
    """Generate an in-memory self-signed cert/key (needs `cryptography`)."""
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    import tempfile

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, u"fieldmesh-mock")])
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.datetime.utcnow() - datetime.timedelta(days=1))
        .not_valid_after(datetime.datetime.utcnow() + datetime.timedelta(days=365))
        .sign(key, hashes.SHA256())
    )
    cdir = tempfile.mkdtemp(prefix="fmmock_")
    cpath, kpath = os.path.join(cdir, "cert.pem"), os.path.join(cdir, "key.pem")
    with open(cpath, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(kpath, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM,
                                  serialization.PrivateFormat.TraditionalOpenSSL,
                                  serialization.NoEncryption()))
    return cpath, kpath


def build_handler(cfg):
    release_base = f"{cfg['storage_prefix']}/{cfg['role']}/{cfg['release_id']}"
    man_path = os.path.join(cfg["release_dir"], "manifest.json")
    sig_path = os.path.join(cfg["release_dir"], "manifest.json.sig")
    img_path = cfg["image_path"]

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def _send(self, code, body, ctype="application/octet-stream"):
            if isinstance(body, str):
                body = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            sys.stderr.write("[mock] " + (fmt % args) + "\n")

        def _envelope(self):
            with LOCK:
                STATE["poll_count"] += 1
                serve = not STATE["served_command"]
            if serve:
                # sequence/cursor=1: the mothership advances its cursor to 1 on
                # accept; subsequent polls with sequence<=cursor are ignored as
                # replays by the firmware, so re-serving is harmless.
                env = {
                    "controlProtocolVersion": 2,
                    "serverTimeUnix": int(datetime.datetime.utcnow().timestamp()),
                    "nextCursor": 1,
                    "commands": [{
                        "commandId": cfg["command_id"],
                        "sequence": 1,
                        "type": "DEPLOY_RELEASE",
                        "expectedStateRevision": 0,
                        "issuedAtUnix": int(datetime.datetime.utcnow().timestamp()) - 60,
                        "expiresAtUnix": int(datetime.datetime.utcnow().timestamp()) + 86400,
                        "payload": {"releaseId": cfg["release_id"]},
                    }],
                }
                sys.stderr.write(f"[mock] --> serving DEPLOY_RELEASE {cfg['release_id']}\n")
            else:
                env = {"controlProtocolVersion": 2,
                       "serverTimeUnix": int(datetime.datetime.utcnow().timestamp()),
                       "nextCursor": 1, "commands": []}
            return json.dumps(env)

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0") or "0")
            _ = self.rfile.read(length)   # ignore the status body
            if self.path.split("?")[0] == cfg["ingest_path"]:
                self._send(200, self._envelope(), "application/json")
            else:
                self._send(404, "no such endpoint")

        def do_GET(self):
            path = self.path.split("?")[0]
            try:
                if path == release_base + "/manifest.json":
                    with open(man_path, "rb") as f:
                        self._send(200, f.read(), "application/json")
                elif path == release_base + "/manifest.json.sig":
                    with open(sig_path, "rb") as f:
                        # firmware trims whitespace; serve the hex line as text
                        self._send(200, f.read(), "text/plain")
                elif path == release_base + "/image.bin":
                    with open(img_path, "rb") as f:
                        data = f.read()
                    stall = cfg["stall_image_sec"]
                    if stall > 0:
                        # Send headers, then go quiet — exercises the firmware's
                        # per-read idle timeout (-> DOWNLOAD_TIMEOUT), not the
                        # full session budget.
                        sys.stderr.write(f"[mock] --> image.bin: headers then STALL {stall}s\n")
                        self.send_response(200)
                        self.send_header("Content-Type", "application/octet-stream")
                        self.send_header("Content-Length", str(len(data)))
                        self.send_header("Connection", "close")
                        self.end_headers()
                        self.wfile.write(data[:64]); self.wfile.flush()
                        __import__("time").sleep(stall)
                        return
                    trunc = cfg["truncate_image"]
                    if trunc > 0 and trunc < len(data):
                        # Declare the full length but send fewer bytes, then close
                        # -> the firmware sees a short stream (-> DOWNLOAD_TRUNCATED).
                        sys.stderr.write(f"[mock] --> image.bin: TRUNCATED {trunc}/{len(data)} bytes\n")
                        self.send_response(200)
                        self.send_header("Content-Type", "application/octet-stream")
                        self.send_header("Content-Length", str(len(data)))
                        self.send_header("Connection", "close")
                        self.end_headers()
                        self.wfile.write(data[:trunc])
                        return
                    sys.stderr.write(f"[mock] --> serving image.bin ({len(data)} bytes)\n")
                    self._send(200, data, "application/octet-stream")
                else:
                    self._send(404, "not found: " + path)
            except FileNotFoundError as e:
                self._send(404, "missing artifact: " + str(e))

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--release-dir", default="release",
                    help="dir holding manifest.json + manifest.json.sig")
    ap.add_argument("--image", default=None,
                    help="path to image.bin (default: <release-dir>/image.bin)")
    ap.add_argument("--release-id", required=True)
    ap.add_argument("--role", default="mothership")
    ap.add_argument("--command-id", default="mock-ota-0001",
                    help="<=23 chars (CMD_ID_LEN=24)")
    ap.add_argument("--ingest-path", default="/ingest",
                    help="path the mothership POSTs its status to")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--certfile", default=None)
    ap.add_argument("--keyfile", default=None)
    ap.add_argument("--stall-image-sec", type=int, default=0,
                    help="serve image.bin headers then go quiet for N s "
                         "(stall leg -> DOWNLOAD_TIMEOUT)")
    ap.add_argument("--truncate-image", type=int, default=0,
                    help="declare full Content-Length but send only N bytes then "
                         "close (interrupt leg -> DOWNLOAD_TRUNCATED)")
    ap.add_argument("--storage-prefix", default="/storage/v1/object/public/releases",
                    help="URL path prefix under which release artifacts are "
                         "served (default: Supabase Storage public-read path "
                         "matching otaReleaseBase() in the firmware)")
    args = ap.parse_args()

    cfg = {
        "release_dir": args.release_dir,
        "image_path": args.image or os.path.join(args.release_dir, "image.bin"),
        "release_id": args.release_id,
        "role": args.role,
        "command_id": args.command_id,
        "ingest_path": args.ingest_path,
        "stall_image_sec": args.stall_image_sec,
        "truncate_image": args.truncate_image,
        "storage_prefix": args.storage_prefix.rstrip("/"),
    }
    if len(args.command_id) > 23:
        sys.exit("--command-id must be <=23 chars (CMD_ID_LEN=24 incl NUL)")
    for p in [os.path.join(cfg["release_dir"], "manifest.json"),
              os.path.join(cfg["release_dir"], "manifest.json.sig"),
              cfg["image_path"]]:
        if not os.path.exists(p):
            sys.exit(f"missing artifact: {p} (run release_sign.py make first)")

    certfile, keyfile = args.certfile, args.keyfile
    if not certfile:
        print("[mock] no --certfile: generating a self-signed cert")
        certfile, keyfile = make_self_signed_cert()

    handler = build_handler(cfg)
    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile, keyfile)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    print(f"[mock] HTTPS on :{args.port}")
    print(f"[mock]   POST {args.ingest_path}  -> DEPLOY_RELEASE {args.release_id} (once, then empty)")
    print(f"[mock]   GET  {args.storage_prefix}/{args.role}/{args.release_id}/{{manifest.json,.sig,image.bin}}")
    print("[mock] expose with a public HTTPS tunnel; point a bench firmware build at that host.")
    print("[mock] Ctrl-C to stop. When the mothership stops re-fetching, the install has taken.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[mock] bye")


if __name__ == "__main__":
    main()
