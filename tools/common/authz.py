"""
authz.py -- accounts, roles and sessions for the S2S2 web tools.

Roles: viewer (read only) < operator (send commands) < admin (users, serial port, critical commands). Passwords are stored as
PBKDF2-SHA256 hashes only (users file mode 600). On the first run an "admin" account with a random password is created and the password
is printed ONCE. Sessions are random tokens in an HttpOnly cookie that expire after 12 hours of absolute lifetime.
Critical commands (antenna release, camera, ...) additionally ask the operator for the password again (`verify`).

The testbench uses the same class with a single admin (`python3 server.py --auth`).
"""
import hashlib
import hmac
import json
import os
import secrets
import time

ROLES = ("viewer", "operator", "admin")
RANK = {r: i for i, r in enumerate(ROLES)}


def _hash(pw, salt):
    return hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, 200_000).hex()


class Auth:
    def __init__(self, path, enabled=True, tool="the tool"):
        self.enabled = enabled
        self.path = path
        self.sessions = {}
        self.users = {}
        self.fails = {}                          # user -> (count, first failure time): lock out brute force
        if not enabled:
            return
        try:
            self.users = json.load(open(path))
            for u in self.users.values():
                u.setdefault("role", "admin")
        except (OSError, ValueError):
            pw = secrets.token_urlsafe(9)
            self.set_password("admin", pw, "admin")
            print("\n  %s: login created: user 'admin'  password %s\n  (shown once, only a hash is stored in %s)\n" % (tool, pw, path))

    def _save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        with open(self.path, "w") as f:
            json.dump(self.users, f)
        os.chmod(self.path, 0o600)

    def set_password(self, user, pw, role=None):
        if len(pw) < 8:
            raise ValueError("password must have at least 8 characters")
        salt = secrets.token_bytes(16)
        old = self.users.get(user, {})
        self.users[user] = {"salt": salt.hex(), "hash": _hash(pw, salt), "role": role or old.get("role", "operator")}
        self._save()

    def delete_user(self, user):
        if user == "admin":
            raise ValueError("the admin account cannot be deleted")
        self.users.pop(user, None)
        self.sessions = {t: s for t, s in self.sessions.items() if s[0] != user}
        self._save()

    def list_users(self):
        return [{"user": k, "role": v["role"]} for k, v in sorted(self.users.items())]

    def verify(self, user, pw):
        u = self.users.get(user)
        n, t0 = self.fails.get(user, (0, 0))
        if n >= 5 and time.time() - t0 < 300:
            return False                          # locked for 5 minutes after 5 wrong passwords
        ok = bool(u) and hmac.compare_digest(_hash(pw, bytes.fromhex(u["salt"])), u["hash"])
        if ok:
            self.fails.pop(user, None)
        else:
            self.fails[user] = (n + 1 if time.time() - t0 < 300 else 1, t0 if n and time.time() - t0 < 300 else time.time())
        return ok

    def login(self, user, pw):
        if not self.verify(user, pw):
            return None
        tok = secrets.token_urlsafe(24)
        self.sessions[tok] = (user, time.time() + 12 * 3600)
        return tok

    def user_of(self, tok):
        s = self.sessions.get(tok or "")
        if not s or s[1] < time.time():
            self.sessions.pop(tok or "", None)
            return None
        return s[0]

    def role_of(self, user):
        return self.users.get(user, {}).get("role", "viewer")

    def allows(self, user, role):
        return RANK[self.role_of(user)] >= RANK[role]

    def logout(self, tok):
        self.sessions.pop(tok or "", None)
