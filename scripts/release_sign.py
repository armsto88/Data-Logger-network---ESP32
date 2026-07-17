#!/usr/bin/env python3
"""FieldMesh release manifest builder + Ed25519 signer (host side).

Signing model (see FIELDMESH_OTA_FIRMWARE_UPDATE_PLAN.md §5, §11):
  - The release private key lives ONLY on the build machine.
  - The matching 32-byte public key is embedded in device firmware.
  - The signature is DETACHED and covers the exact bytes of manifest.json.
    The device verifies the bytes it downloaded, so there is no
    canonicalisation ambiguity (§5.2). manifest.json is emitted compact with
    sorted keys purely for reproducibility; the signature is over whatever
    bytes are written.

Requires: pip install cryptography

Usage:
  release_sign.py keygen  --out-dir keys/
  release_sign.py make    --key keys/fieldmesh_ed25519.pem \\
                          --release-id fieldmesh-2026.08.0 --sequence 12 \\
                          --artifact role=node,version=0.2.0,hw=node-v3,\\
                                     proto=2,min-mothership=0.1.0,\\
                                     bin=.pio/build/esp32wroom/firmware.bin \\
                          --out-dir release/
  release_sign.py sign    --key keys/fieldmesh_ed25519.pem \\
                          --manifest release/manifest.json
  release_sign.py pubkey  --key keys/fieldmesh_ed25519.pem   # C array for firmware
"""
import argparse
import hashlib
import json
import os
import sys

from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey)
from cryptography.hazmat.primitives import serialization


def _load_key(path):
    with open(path, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)


def _pub_raw(priv):
    return priv.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def _canonical(obj):
    # Compact + sorted for reproducible output. The signature is over these
    # exact bytes; the device verifies the same bytes without re-serialising.
    return json.dumps(obj, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False).encode("utf-8")


def cmd_keygen(args):
    os.makedirs(args.out_dir, exist_ok=True)
    priv = Ed25519PrivateKey.generate()
    pem = priv.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption())
    priv_path = os.path.join(args.out_dir, "fieldmesh_ed25519.pem")
    with open(priv_path, "wb") as f:
        f.write(pem)
    pub_hex = _pub_raw(priv).hex()
    with open(os.path.join(args.out_dir, "fieldmesh_ed25519.pub.hex"), "w") as f:
        f.write(pub_hex + "\n")
    print(f"wrote {priv_path}  (KEEP PRIVATE — never commit or ship)")
    print(f"public key (embed in firmware): {pub_hex}")


def _parse_artifact(spec, sign_priv):
    kv = dict(part.split("=", 1) for part in spec.split(","))
    with open(kv["bin"], "rb") as f:
        data = f.read()
    art = {
        "role": kv["role"],
        "version": kv["version"],
        "hwTargets": [h for h in kv["hw"].split("|") if h],
        "buildId": kv.get("build", "nogit"),
        "protocolVersion": int(kv.get("proto", "0")),
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
    if "min-mothership" in kv:
        art["minMothershipVersion"] = kv["min-mothership"]
    return art


def cmd_make(args):
    priv = _load_key(args.key)
    artifacts = [_parse_artifact(a, priv) for a in args.artifact]
    manifest = {
        "schemaVersion": 1,
        "releaseId": args.release_id,
        "releaseSequence": args.sequence,
        "artifacts": artifacts,
    }
    os.makedirs(args.out_dir, exist_ok=True)
    man_path = os.path.join(args.out_dir, "manifest.json")
    raw = _canonical(manifest)
    with open(man_path, "wb") as f:
        f.write(raw)
    sig = priv.sign(raw)
    with open(man_path + ".sig", "w") as f:
        f.write(sig.hex() + "\n")
    print(f"wrote {man_path} ({len(raw)} bytes) + manifest.json.sig")
    for a in artifacts:
        print(f"  {a['role']:10} v{a['version']:8} {a['size']:>8} B  sha256={a['sha256'][:16]}...")


def cmd_sign(args):
    priv = _load_key(args.key)
    with open(args.manifest, "rb") as f:
        raw = f.read()
    sig = priv.sign(raw)
    with open(args.manifest + ".sig", "w") as f:
        f.write(sig.hex() + "\n")
    print(f"signed {args.manifest} -> {args.manifest}.sig")


def cmd_pubkey(args):
    priv = _load_key(args.key)
    raw = _pub_raw(priv)
    arr = ", ".join(f"0x{b:02x}" for b in raw)
    print("// FieldMesh release verification public key (Ed25519, 32 bytes)")
    print(f"static const uint8_t kReleasePubKey[32] = {{ {arr} }};")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("keygen"); g.add_argument("--out-dir", default="keys")
    g.set_defaults(func=cmd_keygen)

    m = sub.add_parser("make")
    m.add_argument("--key", required=True)
    m.add_argument("--release-id", required=True)
    m.add_argument("--sequence", type=int, required=True)
    m.add_argument("--artifact", action="append", required=True,
                   help="role=..,version=..,hw=a|b,proto=..,min-mothership=..,bin=path")
    m.add_argument("--out-dir", default="release")
    m.set_defaults(func=cmd_make)

    s = sub.add_parser("sign")
    s.add_argument("--key", required=True); s.add_argument("--manifest", required=True)
    s.set_defaults(func=cmd_sign)

    k = sub.add_parser("pubkey"); k.add_argument("--key", required=True)
    k.set_defaults(func=cmd_pubkey)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    sys.exit(main())
