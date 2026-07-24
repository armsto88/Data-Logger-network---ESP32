#!/usr/bin/env python3
"""FieldMesh cloud OTA bench — sign, publish, enqueue, in one command.

Ports the Node bench kit to Python (stdlib + cryptography, which is already a
dependency of scripts/release_sign.py) so the firmware team can drive the whole
publish+enqueue against the real Supabase backend without round-tripping
through the dashboard team.

What this does, in order:
  1. Signs image.bin with the throwaway bench Ed25519 key -> manifest.json +
     manifest.json.sig (detached, 128 lowercase hex over the EXACT manifest
     bytes; matches scripts/release_sign.py's output shape, Appendix B §5.6).
  2. Uploads the three artifacts to the public `releases` Storage bucket at
     releases/mothership/<releaseId>/<artifact> with immutable cache headers,
     via the release-catalog edge function (which validates manifest fields,
     image SHA-256, and signature shape, then inserts the catalog row).
  3. (Optional) Calls enqueue_deploy_release for a hub, with the hub's current
     stateRevision read from mothership_control_state (compare-and-set).
  4. Verifies the three Storage public URLs are reachable over public Storage.

Requires (env):
  SUPABASE_URL               https://<id>.supabase.co
  SUPABASE_SERVICE_ROLE_KEY  service-role key (bypasses RLS; never commit it)

Requires a bench keypair at scripts/ota-bench/bench-key.json. Generate one with
  python scripts/release_sign.py keygen --out-dir scripts/ota-bench/keys
and rename/convert to bench-key.json as {"privateKey": "<64 hex>", "publicKey": "<64 hex>"}.
The matching public key must be pasted into kReleasePubKey in
mothership_selfupdate.cpp for the bench build.

Usage:
  python scripts/ota-bench/publish_bench_release.py \\
    --image path/to/image.bin \\
    --releaseId fieldmesh-bench-2026.07.0 \\
    --releaseSequence 1 \\
    --version 0.2.0 --buildId bench-001 \\
    --hwTarget mothership-v1 --protocolVersion 2 \\
    --publish \\
    [--mothershipId <hub-uuid>]   # omit to skip the enqueue step

Node firmware OTA is out of scope; this is mothership-only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import secrets
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path

# cryptography is already a dependency of scripts/release_sign.py.
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--image", required=True, help="path to image.bin")
    ap.add_argument("--releaseId", required=True, help="releaseId (<=39 chars)")
    ap.add_argument("--releaseSequence", type=int, required=True, help="monotonic per role; use > 0")
    ap.add_argument("--version", required=True, help="semantic version, e.g. 0.2.0")
    ap.add_argument("--buildId", required=True, help="git build id")
    ap.add_argument("--hwTarget", default="mothership-v1")
    ap.add_argument("--protocolVersion", type=int, default=2)
    ap.add_argument("--minMothershipVersion", default="0.0.0")
    ap.add_argument("--role", default="mothership", help="mothership only; node OTA out of scope")
    ap.add_argument("--publish", action="store_true", help="transition the catalog row to PUBLISHED")
    ap.add_argument(
        "--direct",
        action="store_true",
        help="bypass the release-catalog edge function (which can be blocked by "
             "Cloudflare WAF on multipart uploads) and publish directly via the "
             "Storage + Postgres REST APIs",
    )
    ap.add_argument(
        "--enqueue-only",
        action="store_true",
        help="skip signing + publishing (assumes the release is already published) "
             "and ONLY call enqueue_deploy_release. Useful after a successful publish "
             "when the hub has since checked in and mothership_control_state has a "
             "state_revision to compare-and-set against.",
    )
    ap.add_argument("--mothershipId", help="hub UUID; if set, enqueue DEPLOY_RELEASE after publish")
    ap.add_argument("--mac", help="hub MAC address (e.g. 48:9D:31:F8:16:A8); if set, resolves the hub UUID by MAC lookup and uses it as --mothershipId. Overrides --mothershipId.")
    ap.add_argument("--benchKey", default="scripts/ota-bench/bench-key.json", help="path to bench keypair json")
    ap.add_argument("--outDir", default="scripts/ota-bench/out", help="where to write signed artifacts")
    args = ap.parse_args()

    if args.role != "mothership":
        print(f"ERROR: role must be 'mothership' (node fleet OTA is out of scope), got {args.role!r}", file=sys.stderr)
        return 2

    url = os.environ.get("SUPABASE_URL", "").rstrip("/")
    service_key = os.environ.get("SUPABASE_SERVICE_ROLE_KEY", "")
    if not url or not service_key:
        print("ERROR: set SUPABASE_URL and SUPABASE_SERVICE_ROLE_KEY in the environment", file=sys.stderr)
        return 1
    if len(args.releaseId) < 1 or len(args.releaseId) > 39:
        print("ERROR: releaseId must be 1-39 chars", file=sys.stderr)
        return 2

    here = Path(__file__).resolve().parent
    repo_root = here.parent.parent
    bench_key_path = (
        Path(args.benchKey) if Path(args.benchKey).is_absolute()
        else repo_root / args.benchKey
    )
    if not bench_key_path.exists():
        print(
            f"ERROR: bench key not found at {bench_key_path}.\n"
            f"Generate one with:\n"
            f"  python scripts/release_sign.py keygen --out-dir scripts/ota-bench/keys\n"
            f"Then create {bench_key_path} with contents: "
            '{"privateKey": "<64 hex>", "publicKey": "<64 hex>"}',
            file=sys.stderr,
        )
        return 1
    bench_key = json.loads(bench_key_path.read_text())
    priv_hex = bench_key["privateKey"]
    if len(priv_hex) != 64:
        print("ERROR: bench-key.json privateKey must be 32 bytes (64 hex)", file=sys.stderr)
        return 1

    # --- 1. Sign -----------------------------------------------------------
    # Skipped in --enqueue-only mode (the release is assumed already published).
    image_bytes = b""
    image_sha256 = ""
    image_size = 0
    manifest_bytes = b""
    sig_hex = ""
    release = None

    if not args.enqueue_only:
        image_bytes = Path(args.image).read_bytes()
        image_sha256 = hashlib.sha256(image_bytes).hexdigest()
        image_size = len(image_bytes)

        manifest = {
            "releaseId": args.releaseId,
            "releaseSequence": args.releaseSequence,
            "schemaVersion": 1,
            "artifacts": [{
                "role": args.role,
                "hwTargets": [args.hwTarget],
                "protocolVersion": args.protocolVersion,
                "version": args.version,
                "buildId": args.buildId,
                "minMothershipVersion": args.minMothershipVersion,
                "sha256": image_sha256,
                "size": image_size,
            }],
        }
        # Compact + sorted for reproducible output. The signature is over these
        # exact bytes; the cloud serves these EXACT bytes — do not re-serialize.
        manifest_bytes = json.dumps(
            manifest, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        ).encode("utf-8")

        sig_bytes = sign_ed25519(priv_hex, manifest_bytes)
        sig_hex = sig_bytes.hex()
        if len(sig_hex) != 128:
            print(f"ERROR: unexpected signature length {len(sig_hex)} (expected 128 hex)", file=sys.stderr)
            return 1

        out_dir = (
            (Path(args.outDir) if Path(args.outDir).is_absolute() else repo_root / args.outDir)
            / args.releaseId
        )
        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / "manifest.json").write_bytes(manifest_bytes)
        (out_dir / "manifest.json.sig").write_text(sig_hex, encoding="utf-8")
        (out_dir / "image.bin").write_bytes(image_bytes)
        print(f"Signed bench release {args.releaseId} (sequence {args.releaseSequence}) into {out_dir}")
        print(f"  manifest.json      {len(manifest_bytes)} bytes")
        print(f"  manifest.json.sig  {len(sig_hex)} hex chars")
        print(f"  image.bin          {image_size} bytes  sha256={image_sha256}")

    headers = {
        "apikey": service_key,
        "Authorization": f"Bearer {service_key}",
    }

    # --- 2+3. Publish ------------------------------------------------------
    # Skipped in --enqueue-only mode (assumes already published).
    # Two equivalent paths the backend documented:
    #   - Edge function (default): validates manifest fields, image SHA-256,
    #     and signature shape; uploads to Storage + inserts the catalog row.
    #     Can be blocked by Cloudflare WAF on multipart uploads (use --direct).
    #   - Direct (--direct): POST each artifact to Storage with immutable cache
    #     headers, then POST the firmware_releases row via Postgres REST.
    # firmware_releases.uploaded_by is NOT NULL, so for the direct path we need
    # a user_id up front. Resolve it once (from the hub's project owner) before
    # publishing; reuse it for uploaded_by and the enqueue RPC's p_user_id.
    user_id, project_id = resolve_project_owner(url, headers, args.mothershipId, args.mac)
    if args.direct and not user_id:
        print("ERROR: could not resolve a project owner (needed for uploaded_by). "
              "Pass --mothershipId or --mac for a hub that exists in the motherships table.",
              file=sys.stderr)
        return 1

    if args.enqueue_only:
        print(f"\n--enqueue-only: skipping sign + publish (release {args.releaseId} assumed published).")
    elif args.direct:
        print("\nPublishing via direct Storage + Postgres REST (--direct)...")
        release = publish_direct(
            url=url,
            headers=headers,
            role=args.role,
            release_id=args.releaseId,
            release_sequence=args.releaseSequence,
            version=args.version,
            build_id=args.buildId,
            hw_target=args.hwTarget,
            protocol_version=args.protocolVersion,
            min_mothership_version=args.minMothershipVersion,
            manifest_bytes=manifest_bytes,
            sig_hex=sig_hex,
            image_bytes=image_bytes,
            image_sha256=image_sha256,
            image_size=image_size,
            publish=args.publish,
            uploaded_by=user_id,
        )
    else:
        print("\nPublishing via release-catalog edge function...")
        release = publish_via_edge_function(
            url=url,
            headers=headers,
            role=args.role,
            release_id=args.releaseId,
            publish=args.publish,
            manifest_bytes=manifest_bytes,
            sig_hex=sig_hex,
            image_bytes=image_bytes,
        )
    if not args.enqueue_only and release is None:
        return 1
    if not args.enqueue_only:
        print(f"  Catalog row: {release['id']} state={release['lifecycleState']}")
        print(
            f"  releaseId={release['releaseId']}  version={release.get('version')}  "
            f"releaseSequence={release['releaseSequence']}  sizeBytes={release.get('sizeBytes')}"
        )

    # --- 4. Verify Storage public URLs are reachable ------------------------
    # Skipped in --enqueue-only mode (already verified during publish).
    if not args.enqueue_only:
        print("\nVerifying Storage public URLs are reachable (no auth):")
        for artifact in ("manifest.json", "manifest.json.sig", "image.bin"):
            u = f"{url}/storage/v1/object/public/releases/{args.role}/{args.releaseId}/{artifact}"
            r = http_head(u)
            print(f"  {r.status}  {u}  ({r.headers.get('content-length') or '?'} bytes)")
            if r.status != 200:
                print("ERROR: artifact not reachable — the firmware won't be able to fetch it either.", file=sys.stderr)
                return 1

    # --- 5. Enqueue (optional) ---------------------------------------------
    # Reuse the hub UUID + project owner already resolved before publishing.
    # If --mac was given and --mothershipId wasn't, resolve_project_owner()
    # already pinned the hub UUID; re-resolve it here so we have it for enqueue.
    mothership_id = args.mothershipId
    if args.mac and not mothership_id:
        mac_norm = args.mac.strip().upper()
        print(f"\nResolving hub UUID by MAC {mac_norm}...")
        mac_res = http_get(
            f"{url}/rest/v1/motherships?mac_address=eq.{mac_norm}&select=id,name",
            headers={**headers, "Accept": "application/json"},
        )
        mac_rows = mac_res.json if mac_res.json else []
        if mac_res.status != 200 or not mac_rows:
            print(f"ERROR: no hub found with MAC {mac_norm}: {mac_res.status} {mac_res.text}", file=sys.stderr)
            return 1
        mothership_id = mac_rows[0]["id"]
        print(f"  hub UUID = {mothership_id}  (name={mac_rows[0].get('name')})")

    if mothership_id:
        # user_id + project_id were resolved before publishing (or re-resolve now
        # if only --mac was given and we didn't resolve them up front).
        if not user_id or not project_id:
            user_id, project_id = resolve_project_owner(url, headers, mothership_id, None)
        if not user_id or not project_id:
            print("ERROR: could not resolve hub/project owner for enqueue.", file=sys.stderr)
            return 1

        print(f"\nReading hub {mothership_id} state for compare-and-set...")
        cs_res = http_get(
            f"{url}/rest/v1/mothership_control_state?mothership_id=eq.{mothership_id}&select=state_revision",
            headers={**headers, "Accept": "application/json"},
        )
        cs_rows = cs_res.json if cs_res.json else []
        if cs_res.status != 200 or not cs_rows:
            print(
                "ERROR: hub has not reported status.control{} yet (state_revision unavailable). "
                "Wait for a check-in, then re-run.",
                file=sys.stderr,
            )
            return 1
        state_revision = cs_rows[0]["state_revision"]
        print(f"  stateRevision = {state_revision}")

        command_id = "D" + secrets.token_urlsafe(16)[:22]
        expires_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() + 7 * 86400))
        rpc_body = json.dumps({
            "p_user_id": user_id,
            "p_project_id": project_id,
            "p_mothership_id": mothership_id,
            "p_command_id": command_id,
            "p_release_id": args.releaseId,
            "p_expected_state_revision": state_revision,
            "p_expires_at": expires_at,
        }).encode("utf-8")
        rpc_res = http_post(
            f"{url}/rest/v1/rpc/enqueue_deploy_release",
            body=rpc_body,
            headers={**headers, "Content-Type": "application/json"},
        )
        if rpc_res.status != 200:
            msg = rpc_res.json.get("message") if rpc_res.json else rpc_res.text
            if "FM_REVISION_CONFLICT" in (msg or ""):
                print(
                    "ERROR: REVISION_CONFLICT — hub moved revision. Re-run; it'll read the new value.",
                    file=sys.stderr,
                )
            elif "FM_COMMAND_ALREADY_PENDING" in (msg or ""):
                print("ERROR: COMMAND_ALREADY_PENDING — another command is in flight. Wait for it to finish.", file=sys.stderr)
            elif "FM_RELEASE_NOT_PUBLISHED" in (msg or ""):
                print("ERROR: RELEASE_NOT_PUBLISHED — re-run with --publish.", file=sys.stderr)
            else:
                print(f"ERROR: enqueue_deploy_release failed: {rpc_res.status} {msg}", file=sys.stderr)
            return 1
        cmd = rpc_res.json
        print("\nEnqueued DEPLOY_RELEASE:")
        print(f"  commandId             = {cmd['commandId']}")
        print(f"  sequence              = {cmd['sequence']}")
        print(f"  releaseId             = {cmd['releaseId']}")
        print(f"  expectedStateRevision = {state_revision}")
        print(f"  state                 = {cmd['state']}")
        print(f"  expiresAt             = {cmd['expiresAt']}")
        print("\nThe hub will pull this on its next LTE check-in. Watch on the device:")
        print("  pio device monitor")
        print("Expect status.firmware{} to walk:")
        print("  pendingReleaseId -> armedReleaseId -> releaseId==target + otaState=CONFIRMED")
    else:
        print("\n(--mothershipId / --mac not given; skipping enqueue. Run again with one of them to enqueue.)")

    print("\nDone.")
    return 0


def resolve_project_owner(url, headers, mothership_id, mac):
    """Resolve (user_id, project_id) for a hub. If --mac is given, look up the
    hub by MAC; else use --mothershipId. Returns (None, None) if not found.
    The user_id is the project owner — needed for firmware_releases.uploaded_by
    (NOT NULL) on the direct publish path, and for the enqueue RPC's p_user_id.
    """
    target = mothership_id
    if mac:
        mac_norm = mac.strip().upper()
        mr = http_get(
            f"{url}/rest/v1/motherships?mac_address=eq.{mac_norm}&select=id,project_id",
            headers={**headers, "Accept": "application/json"},
        )
        rows = mr.json if mr.json else []
        if mr.status != 200 or not rows:
            print(f"WARNING: no hub found for MAC {mac_norm}: {mr.status} {mr.text}", file=sys.stderr)
            return (None, None)
        target = rows[0]["id"]
        project_id = rows[0]["project_id"]
    elif mothership_id:
        hr = http_get(
            f"{url}/rest/v1/motherships?id=eq.{mothership_id}&select=id,project_id",
            headers={**headers, "Accept": "application/json"},
        )
        rows = hr.json if hr.json else []
        if hr.status != 200 or not rows:
            print(f"WARNING: hub {mothership_id} not found: {hr.status} {hr.text}", file=sys.stderr)
            return (None, None)
        project_id = rows[0]["project_id"]
    else:
        return (None, None)

    or_ = http_get(
        f"{url}/rest/v1/projects?id=eq.{project_id}&select=user_id",
        headers={**headers, "Accept": "application/json"},
    )
    orows = or_.json if or_.json else []
    if or_.status != 200 or not orows:
        print(f"WARNING: could not resolve owner for project {project_id}: {or_.status} {or_.text}", file=sys.stderr)
        return (None, project_id)
    return (orows[0]["user_id"], project_id)


def publish_via_edge_function(url, headers, role, release_id, publish,
                               manifest_bytes, sig_hex, image_bytes):
    """Upload via the release-catalog edge function (multipart). Returns the
    release dict on success, None on failure. Can be blocked by Cloudflare WAF
    on multipart uploads — use publish_direct() in that case.
    """
    boundary = "----fieldmeshbench" + secrets.token_hex(8)
    body = bytearray()
    fields = {
        "releaseId": release_id,
        "role": role,
        "publish": "true" if publish else "false",
        "label": "Bench release (throwaway key)",
        "notes": "Signed with the throwaway bench key. Delete after the bench.",
    }
    for name, value in fields.items():
        body += f"--{boundary}\r\n".encode()
        body += f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode()
        body += f"{value}\r\n".encode()
    for name, filename, content, ctype in [
        ("manifest", "manifest.json", manifest_bytes, "application/json"),
        ("signature", "manifest.json.sig", sig_hex.encode("utf-8"), "text/plain"),
        ("image", "image.bin", image_bytes, "application/octet-stream"),
    ]:
        body += f"--{boundary}\r\n".encode()
        body += f'Content-Disposition: form-data; name="{name}"; filename="{filename}"\r\n'.encode()
        body += f"Content-Type: {ctype}\r\n\r\n".encode()
        body += content + b"\r\n"
    body += f"--{boundary}--\r\n".encode()

    fn_res = http_post(
        f"{url}/functions/v1/release-catalog",
        body=bytes(body),
        headers={**headers, "Content-Type": f"multipart/form-data; boundary={boundary}"},
    )
    if fn_res.status != 201 or not (fn_res.json and fn_res.json.get("success")):
        print(f"ERROR: release-catalog upload failed: {fn_res.status} {fn_res.text}", file=sys.stderr)
        return None
    return fn_res.json["release"]


def publish_direct(url, headers, role, release_id, release_sequence, version, build_id,
                   hw_target, protocol_version, min_mothership_version,
                   manifest_bytes, sig_hex, image_bytes, image_sha256, image_size, publish,
                   uploaded_by):
    """Upload artifacts to Storage + insert the firmware_releases row via
    Postgres REST directly, bypassing the edge function. Returns the release
    dict on success, None on failure.

    `uploaded_by` is the project owner's UUID (required — the table column is
    NOT NULL). Resolve it once before calling via resolve_project_owner().

    The manifest sha256/signature sha256 are computed over the exact bytes we
    upload (the firmware verifies the same bytes — no re-serialization).
    """
    manifest_sha = hashlib.sha256(manifest_bytes).hexdigest()
    # The firmware reads the .sig file as TEXT (128 hex chars) and converts to
    # 64 raw bytes via fwHexToBytes(). So we must upload the hex-text form,
    # NOT raw bytes — uploading raw bytes gives a 64-byte file that fails
    # `sigBody.length() < 128` with SIGNATURE_INVALID. Match the edge-function
    # path (which uploads sig_hex.encode("utf-8")).
    sig_text_bytes = sig_hex.encode("utf-8")  # 128 bytes of ASCII hex
    sig_sha = hashlib.sha256(sig_text_bytes).hexdigest()

    cache_headers = {"cacheControl": "public, max-age=31536000, immutable"}
    base_path = f"releases/{role}/{release_id}"
    uploads = [
        (f"{base_path}/manifest.json", manifest_bytes, "application/json"),
        (f"{base_path}/manifest.json.sig", sig_text_bytes, "text/plain"),
        (f"{base_path}/image.bin", image_bytes, "application/octet-stream"),
    ]
    for path, content, ctype in uploads:
        print(f"  Storage upload: {path}  ({len(content)} bytes)")
        up_headers = {
            **headers,
            "Content-Type": ctype,
            "x-upsert": "true",
            **cache_headers,
        }
        res = http_post(
            f"{url}/storage/v1/object/{path}",
            body=content,
            headers=up_headers,
        )
        if res.status not in (200, 201):
            print(f"ERROR: Storage upload failed for {path}: {res.status} {res.text}", file=sys.stderr)
            return None

    # Insert the catalog row via Postgres REST. Column names + types match the
    # firmware_releases schema (confirmed via OpenAPI /rest/v1/ definitions).
    row = {
        "release_id": release_id,
        "role": role,
        "release_sequence": release_sequence,
        "version": version,
        "build_id": build_id,
        "hw_targets": [hw_target],          # text[] — single-element array
        "protocol_version": protocol_version,
        "min_mothership_version": min_mothership_version,
        "manifest_object_path": f"releases/{role}/{release_id}/manifest.json",
        "signature_object_path": f"releases/{role}/{release_id}/manifest.json.sig",
        "image_object_path": f"releases/{role}/{release_id}/image.bin",
        "manifest_sha256": manifest_sha,
        "signature_sha256": sig_sha,
        "image_sha256": image_sha256,
        "image_size_bytes": image_size,
        "lifecycle_state": "PUBLISHED" if publish else "DRAFT",
        "label": "Bench release (throwaway key)",
        "notes": "Signed with the throwaway bench key. Delete after the bench.",
        "uploaded_by": uploaded_by,
        # published_by / published_at / created_at / updated_at
        # are populated by defaults/triggers.
    }
    print(f"  Postgres insert: firmware_releases {release_id}  state={row['lifecycle_state']}")
    pg_res = http_post(
        f"{url}/rest/v1/firmware_releases",
        body=json.dumps(row).encode("utf-8"),
        headers={**headers, "Content-Type": "application/json", "Prefer": "return=representation"},
    )
    if pg_res.status not in (200, 201):
        print(f"ERROR: firmware_releases insert failed: {pg_res.status} {pg_res.text}", file=sys.stderr)
        return None
    rows = pg_res.json if isinstance(pg_res.json, list) else [pg_res.json]
    if not rows:
        print("ERROR: firmware_releases insert returned no rows", file=sys.stderr)
        return None
    r = rows[0]
    # Normalize the release dict so the caller can print common fields.
    return {
        "id": r.get("id"),
        "releaseId": r.get("release_id", release_id),
        "lifecycleState": r.get("lifecycle_state"),
        "version": r.get("version", version),
        "releaseSequence": r.get("release_sequence", release_sequence),
        "sizeBytes": r.get("image_size_bytes", image_size),
    }


def sign_ed25519(priv_hex: str, message: bytes) -> bytes:
    """Sign with the bench Ed25519 private key (raw 32-byte seed in hex).

    Uses Python's cryptography library (already a dependency of
    scripts/release_sign.py), so no Node required.
    """
    # Ed25519 private key from raw 32-byte seed.
    priv = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(priv_hex))
    return priv.sign(message)


class HttpResponse:
    def __init__(self, status, headers, body_bytes):
        self.status = status
        self.headers = headers
        self._body = body_bytes
        self.text = body_bytes.decode("utf-8", errors="replace")
        try:
            self.json = json.loads(self.text)
        except Exception:
            self.json = None


def _request(method, url, body=None, headers=None):
    req = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req) as r:
            return HttpResponse(r.status, dict(r.headers), r.read())
    except urllib.error.HTTPError as e:
        return HttpResponse(e.code, dict(e.headers), e.read())


def http_get(url, headers):
    return _request("GET", url, headers=headers)


def http_head(url):
    return _request("HEAD", url, headers={})


def http_post(url, body, headers):
    return _request("POST", url, body=body, headers=headers)


if __name__ == "__main__":
    sys.exit(main())