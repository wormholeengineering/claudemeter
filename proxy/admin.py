"""
CLI administrativa do ClaudeMeter — roda DENTRO do container e fala com
127.0.0.1:8000 (loopback), que é a única origem aceita sem ADMIN_TOKEN.

    docker exec -i claude-meter python admin.py cookies < cookies.json
    docker exec    claude-meter python admin.py status
    docker exec    claude-meter python admin.py refresh
    docker exec    claude-meter python admin.py balance 12.50
"""

import json
import sys
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:8000"


def call(method: str, path: str, body=None) -> int:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        BASE + path, data=data, method=method,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            print(resp.read().decode())
            return 0
    except urllib.error.HTTPError as e:
        print(f"HTTP {e.code}: {e.read().decode()}", file=sys.stderr)
        return 1


def main(argv: list[str]) -> int:
    cmd = argv[1] if len(argv) > 1 else ""
    if cmd == "cookies":
        return call("POST", "/cookies", json.load(sys.stdin))
    if cmd == "status":
        return call("GET", "/cookies/status")
    if cmd == "refresh":
        return call("POST", "/refresh")
    if cmd == "balance" and len(argv) == 3:
        return call("POST", "/balance", {"value": float(argv[2])})
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
