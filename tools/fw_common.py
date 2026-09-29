"""
Utilidades compartilhadas por flash_device.py e publish_firmware.py.

Roda com o Python do ESP-IDF (tem esptool, espsecure e cryptography):
    C:\\Espressif\\python_env\\idf5.5_py3.11_env\\Scripts\\python.exe tools/...
"""

import hashlib
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FIRMWARE = REPO / "firmware"
BUILD = FIRMWARE / "build"
APP_BIN = BUILD / "claudemeter.bin"
PROJECT = "claudemeter"

APP_DESC_MAGIC = 0xABCD5432
SEMVER = re.compile(r"^\d+\.\d+\.\d+$")


def die(msg: str, code: int = 1) -> None:
    print(f"ERRO: {msg}", file=sys.stderr)
    sys.exit(code)


def run(args: list[str], capture: bool = False) -> str:
    print("$", " ".join(str(a) for a in args))
    res = subprocess.run([str(a) for a in args], text=True,
                         capture_output=capture)
    if capture:
        out = (res.stdout or "") + (res.stderr or "")
        if res.returncode != 0:
            print(out)
            die(f"comando falhou ({res.returncode})")
        return out
    if res.returncode != 0:
        die(f"comando falhou ({res.returncode})")
    return ""


def esptool(*args) -> list[str]:
    return [sys.executable, "-m", "esptool", *args]


def espsecure(*args) -> list[str]:
    return [sys.executable, "-m", "espsecure", *args]


def version_txt() -> str:
    v = (FIRMWARE / "version.txt").read_text(encoding="utf-8").strip()
    if not SEMVER.match(v):
        die(f"firmware/version.txt inválido: {v!r} (esperado X.Y.Z)")
    return v


def semver(v: str) -> tuple[int, int, int]:
    if not SEMVER.match(v):
        die(f"versão inválida: {v!r}")
    return tuple(int(x) for x in v.split("."))  # type: ignore[return-value]


def app_desc(path: Path) -> dict:
    """Lê esp_app_desc_t do início da imagem (logo após os headers)."""
    data = path.read_bytes()[:256]
    # image header (24 B) + primeiro segment header (8 B)
    off = 24 + 8
    magic, secure_ver = struct.unpack_from("<II", data, off)
    if magic != APP_DESC_MAGIC:
        die(f"{path.name}: esp_app_desc não encontrado")
    version = data[off + 16: off + 48].split(b"\0")[0].decode()
    project = data[off + 48: off + 80].split(b"\0")[0].decode()
    return {"version": version, "project": project, "secure_version": secure_ver}


def ota_slot_size() -> int:
    """Tamanho do slot ota_0 em partitions.csv."""
    for line in (FIRMWARE / "partitions.csv").read_text(encoding="utf-8").splitlines():
        cols = [c.strip() for c in line.split("#")[0].split(",")]
        if len(cols) >= 5 and cols[0] == "ota_0":
            return int(cols[4], 0)
    die("ota_0 não encontrado em partitions.csv")
    return 0


def check_signing_key(key: Path) -> Path:
    key = key.expanduser().resolve()
    if not key.is_file():
        die(f"chave de assinatura não encontrada: {key}")
    try:
        key.relative_to(REPO)
        die("a chave de assinatura não pode ficar dentro do repositório")
    except ValueError:
        pass
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.hazmat.primitives.serialization import load_pem_private_key
    k = load_pem_private_key(key.read_bytes(), password=None)
    if not isinstance(k, rsa.RSAPrivateKey) or k.key_size != 3072:
        die("a chave precisa ser RSA-3072 (espsecure generate_signing_key --version 2 --scheme rsa3072)")
    return key


def signing_key_arg(value: str | None) -> Path:
    value = value or os.environ.get("CLAUDEMETER_SIGNING_KEY")
    if not value:
        die("informe --key ou CLAUDEMETER_SIGNING_KEY (chave RSA-3072 fora do repo)")
    return check_signing_key(Path(value))


def sign_app(key: Path, out: Path) -> Path:
    """Assina build/claudemeter.bin (Secure Boot V2 / RSA-3072) e verifica."""
    if not APP_BIN.is_file():
        die(f"{APP_BIN} não existe — rode idf.py build")
    desc = app_desc(APP_BIN)
    ver = version_txt()
    if desc["project"] != PROJECT or desc["version"] != ver:
        die(f"imagem é {desc['project']} {desc['version']}, esperado {PROJECT} {ver} "
            "(rebuild após alterar version.txt?)")
    out.parent.mkdir(parents=True, exist_ok=True)
    run(espsecure("sign_data", "--version", "2", "--keyfile", key,
                  "--output", out, APP_BIN))
    run(espsecure("verify_signature", "--version", "2", "--keyfile", key, out))
    if out.stat().st_size > ota_slot_size():
        die(f"imagem assinada ({out.stat().st_size} B) maior que o slot OTA")
    return out


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()
