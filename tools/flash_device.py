"""
Gravação física inicial do ClaudeMeter (USB) com a tabela OTA.

    python tools/flash_device.py --port COM12 --key D:\\chaves\\claudemeter_signing.pem
    python tools/flash_device.py --port COM12 --key ... --erase-and-flash

1. esptool flash_id — PARA se a flash não for 4 MB (nada é apagado/gravado).
2. Assina build/claudemeter.bin (a imagem inicial também precisa estar
   assinada: o OTA verifica novas imagens com a chave da imagem em execução).
3. Sem --erase-and-flash: só mostra o plano.
   Com --erase-and-flash: erase_flash + write_flash (bootloader, tabela,
   otadata inicial e app assinado em ota_0). Apaga a NVS.
"""

import argparse
import re
import sys

from fw_common import (BUILD, die, esptool, run, sign_app, signing_key_arg,
                       version_txt)

REQUIRED_FLASH = "4MB"


def detect_flash_size(output: str) -> str | None:
    m = re.search(r"Detected flash size:\s*(\S+)", output)
    return m.group(1) if m else None


def flash_plan(signed_app) -> tuple[list[str], list[str]]:
    """Lê build/flash_args e troca o app pelo binário assinado."""
    lines = (BUILD / "flash_args").read_text(encoding="utf-8").split("\n")
    opts = lines[0].split()
    pairs: list[str] = []
    for line in lines[1:]:
        if not line.strip():
            continue
        addr, path = line.split()
        if path == "claudemeter.bin":
            pairs += [addr, str(signed_app)]
        else:
            pairs += [addr, str(BUILD / path)]
    return opts, pairs


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--key", help="chave RSA-3072 (ou CLAUDEMETER_SIGNING_KEY)")
    ap.add_argument("--erase-and-flash", action="store_true",
                    help="apaga a flash inteira e grava (sem isso: só o plano)")
    args = ap.parse_args()

    key = signing_key_arg(args.key)

    # 1) Confirmação física da flash — condição para tudo o que segue
    out = run(esptool("--chip", "esp32c3", "--port", args.port, "flash_id"), capture=True)
    print(out)
    size = detect_flash_size(out)
    if size != REQUIRED_FLASH:
        print("\n" + "=" * 64)
        print(f"PARE: flash detectada = {size or 'desconhecida'}; a tabela OTA exige {REQUIRED_FLASH}.")
        print("Nada foi apagado nem gravado. Reporte antes de continuar.")
        print("=" * 64)
        sys.exit(2)
    print(f"OK: flash de {size} confirmada.")

    # 2) App assinado
    ver = version_txt()
    signed = sign_app(key, BUILD / "signed" / f"claudemeter-{ver}.bin")
    opts, pairs = flash_plan(signed)

    cmd = esptool("--chip", "esp32c3", "--port", args.port, "-b", "460800",
                  "--before", "default_reset", "--after", "hard_reset",
                  "write_flash", *opts, *pairs)
    print("\nPlano:")
    print("  erase_flash (apaga tudo, inclusive NVS)")
    print("  " + " ".join(cmd[3:]))

    if not args.erase_and_flash:
        print("\nNada gravado. Repita com --erase-and-flash para executar.")
        return

    # 3) Gravação
    run(esptool("--chip", "esp32c3", "--port", args.port, "erase_flash"))
    run(cmd)
    print(f"\nClaudeMeter {ver} gravado (ota_0, assinado).")


if __name__ == "__main__":
    main()
