"""Decodes (and encodes) GT7's online service ("Vegas") message bodies.

GT7 (CUSA24769, client 5.6.0) sends its API requests to api.<stage>.vegas.granturismo-online.net
with Content-Type application/octet-stream. A body is:

  bytes 0-1    01 01          format version
  byte  2      0x00 / 0x80    0x80 when the payload is NOT zstd-compressed (small payloads)
  byte  3      00
  bytes 4-15   ChaCha20 nonce (12 random bytes; IETF ChaCha20, 20 rounds, counter 0)
  bytes 16-47  HMAC-SHA256 of the ciphertext (bytes 48-) with HMAC_KEY
  bytes 48-    ChaCha20(CIPHER_KEY, nonce) of the payload: a zstd frame of JSON
               {"body": {...}, "headers": {app_name, app_version, client_version, sequence_no,
                client_request_id, window_name, boot_session_id, ...}}

Both keys are base64 strings in GT7's executable (next to "VegasPoolTransport"). They are not in
this file (FIX-GTS-271): give your own GT7 eboot.bin with --eboot (or RYGT_GT7_EBOOT), and the keys
are picked out of it by their SHA-256 fingerprints. Responses are assumed to use the same format.

Usage: vegas_decode.py --eboot <eboot.bin> <body file>...         prints each payload
       vegas_decode.py --eboot <eboot.bin> --encode <json file>   writes <json file>.bin (needs zstd)"""
import base64, hashlib, hmac, os, re, subprocess, sys, tempfile

from Crypto.Cipher import ChaCha20

CIPHER_KEY_SHA256 = "690168cbeccc3e7f9e4b19798d4b317c5a75236df2845f25449b890f0b1ecf78"
HMAC_KEY_SHA256 = "6524bc4c832361d8bcd806f4a322e825cffcf19ec1d81e935dad390d01783c83"
CIPHER_KEY = HMAC_KEY = None
HEADER = 48


def load_keys(executable):
    """Picks GT7's two keys out of its executable: base64 runs whose SHA-256 is a key's fingerprint."""
    global CIPHER_KEY, HMAC_KEY
    with open(executable, "rb") as f:
        data = f.read()
    for match in re.finditer(rb"(?<![A-Za-z0-9+/=])(?:[A-Za-z0-9+/]{43}=|[A-Za-z0-9+/]{86}==)(?![A-Za-z0-9+/=])", data):
        key = base64.b64decode(match.group(0))
        fingerprint = hashlib.sha256(key).hexdigest()
        if fingerprint == CIPHER_KEY_SHA256:
            CIPHER_KEY = key
        elif fingerprint == HMAC_KEY_SHA256:
            HMAC_KEY = key
    if CIPHER_KEY is None or HMAC_KEY is None:
        raise SystemExit(executable + " has no GT7 online keys")


def zstd(data, decompress):
    with tempfile.TemporaryDirectory() as folder:
        source, target = os.path.join(folder, "in"), os.path.join(folder, "out")
        with open(source, "wb") as f:
            f.write(data)
        subprocess.run(["zstd", "-qf", "-d" if decompress else "-3", source, "-o", target], check=True)
        with open(target, "rb") as f:
            return f.read()


def decode(body):
    if len(body) < HEADER or body[:2] != b"\x01\x01":
        raise ValueError("not a Vegas body")
    nonce, tag, ciphertext = body[4:16], body[16:48], body[48:]
    if not hmac.compare_digest(hmac.new(HMAC_KEY, ciphertext, hashlib.sha256).digest(), tag):
        raise ValueError("HMAC mismatch")
    payload = ChaCha20.new(key=CIPHER_KEY, nonce=nonce).decrypt(ciphertext)
    return payload if body[2] & 0x80 else zstd(payload, True)


def encode(payload, compress=True):
    nonce = os.urandom(12)
    ciphertext = ChaCha20.new(key=CIPHER_KEY, nonce=nonce).encrypt(zstd(payload, False) if compress else payload)
    flags = b"\x00" if compress else b"\x80"
    return b"\x01\x01" + flags + b"\x00" + nonce + hmac.new(HMAC_KEY, ciphertext, hashlib.sha256).digest() + ciphertext


def main(args):
    eboot = os.environ.get("RYGT_GT7_EBOOT")
    if args[:1] == ["--eboot"] and len(args) > 1:
        eboot, args = args[1], args[2:]
    if not eboot:
        raise SystemExit(__doc__)
    load_keys(eboot)
    if args[:1] == ["--encode"]:
        for name in args[1:]:
            with open(name, "rb") as f, open(name + ".bin", "wb") as out:
                out.write(encode(f.read()))
        return
    for name in args:
        with open(name, "rb") as f:
            print("==", name)
            print(decode(f.read()).decode("utf-8", "replace"))


if __name__ == "__main__":
    main(sys.argv[1:])
