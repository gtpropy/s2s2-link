# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-09-01
"""
auth.py -- password hashing + signed session tokens for
mission_control_api.py. Stdlib only (hmac/hashlib/secrets), no new
dependency -- matches this project's minimal-dependency convention
elsewhere (gmsk_demod.py: "numpy + scipy only", ml_denoise.py: no ML
framework). PBKDF2-HMAC-SHA256 for passwords (slow-by-design, unlike a
bare sha256/md5 hash -- that distinction matters once this is reachable
from a real domain, not just a bench laptop). Sessions are a signed,
tamper-evident cookie (HMAC-SHA256 over user_id+expiry), not a bare
lookup token -- no server-side session store needed, and a forged
cookie fails verification rather than silently working.

SECRET_KEY is generated once (secrets.token_bytes(32)) and persisted to
`.session_secret` next to this file, gitignored -- never commit it or
share it between deployments; rotating it invalidates every session.
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import secrets
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SECRET_KEY_PATH = os.path.join(HERE, ".session_secret")

PBKDF2_ITERATIONS = 210_000
SESSION_TTL_SECONDS = 12 * 3600  # 12h -- re-login after that, not "forever"


def _load_or_create_secret_key() -> bytes:
    if os.path.isfile(SECRET_KEY_PATH):
        with open(SECRET_KEY_PATH, "rb") as f:
            key = f.read()
        if len(key) == 32:
            return key
    key = secrets.token_bytes(32)
    with open(SECRET_KEY_PATH, "wb") as f:
        f.write(key)
    try:
        os.chmod(SECRET_KEY_PATH, 0o600)
    except OSError:
        pass
    return key


SECRET_KEY = _load_or_create_secret_key()


# =====================================================================
# Passwords
# =====================================================================

def hash_password(password: str, salt: bytes = None) -> str:
    """Returns "salt_hex$hash_hex". Verified constant-time by
    verify_password(); never compare hashes with `==` directly."""
    if salt is None:
        salt = secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, PBKDF2_ITERATIONS)
    return f"{salt.hex()}${digest.hex()}"


def verify_password(password: str, stored: str) -> bool:
    try:
        salt_hex, digest_hex = stored.split("$", 1)
    except ValueError:
        return False
    salt = bytes.fromhex(salt_hex)
    candidate = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, PBKDF2_ITERATIONS)
    return hmac.compare_digest(candidate.hex(), digest_hex)


# =====================================================================
# Sessions -- signed cookie, no server-side store
# =====================================================================

def create_session_token(user_id: int) -> str:
    payload = {"uid": user_id, "exp": time.time() + SESSION_TTL_SECONDS}
    body = base64.urlsafe_b64encode(json.dumps(payload).encode("utf-8")).rstrip(b"=")
    sig = hmac.new(SECRET_KEY, body, hashlib.sha256).digest()
    sig_b64 = base64.urlsafe_b64encode(sig).rstrip(b"=")
    return f"{body.decode()}.{sig_b64.decode()}"


def verify_session_token(token: str):
    """Returns user_id, or None if missing/expired/tampered."""
    if not token or "." not in token:
        return None
    body_b64, _, sig_b64 = token.partition(".")
    try:
        body = base64.urlsafe_b64decode(body_b64 + "==")
        sig = base64.urlsafe_b64decode(sig_b64 + "==")
    except Exception:
        return None
    expected_sig = hmac.new(SECRET_KEY, body_b64.encode(), hashlib.sha256).digest()
    if not hmac.compare_digest(sig, expected_sig):
        return None
    try:
        payload = json.loads(body)
    except Exception:
        return None
    if payload.get("exp", 0) < time.time():
        return None
    return payload.get("uid")


# =====================================================================
# Critical-command fingerprint -- sha256(secret) compared to a stored
# fingerprint (never the plaintext secret itself, see mission_control_api.py's
# settings table). Distinct from password hashing on purpose: this is a
# shared operational passphrase (like a launch-key), not a per-account
# credential, so a fast hash is the right tool here, not PBKDF2 -- the
# threat model is "did the caller know the secret", not "resist offline
# brute force of a leaked hash database".
# =====================================================================

def fingerprint(secret: str) -> str:
    return hashlib.sha256(secret.encode("utf-8")).hexdigest()
