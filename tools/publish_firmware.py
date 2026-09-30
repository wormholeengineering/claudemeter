"""
Publica uma versão de firmware para FOTA em watch.jimmylab.com.br/firmware/.

    python tools/publish_firmware.py --key D:\\chaves\\claudemeter_signing.pem --dry-run
    python tools/publish_firmware.py --key ... --ssh jimmycl@jimmylab --identity ~/.ssh/aria_pi

- Versão = firmware/version.txt (precisa bater com o esp_app_desc do build).
- Assina com RSA-3072 (espsecure) e verifica a assinatura.
- Gera manifest.json com versão, url, tamanho e SHA-256.
- Envia primeiro o .bin e só depois o manifest, cada um como .tmp + mv
  (atômico): o dispositivo nunca vê manifest apontando para .bin ausente.
- Binários são imutáveis: aborta se já existir o mesmo nome com outro hash.

Em duas etapas (validar o .bin pelo watch antes de expor o manifest):
    --step bin       assina, gera o manifest local e envia só o .bin
    --step manifest  reusa o .bin já assinado (sem reassinar: a assinatura
                     RSA-PSS é aleatória) e publica o manifest por último
"""

import argparse
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path

import fw_common as fw
from fw_common import (PROJECT, app_desc, die, espsecure, run, semver,
                       sha256, sign_app, signing_key_arg, version_txt)

REMOTE_DIR = "/docker/apps/claude-meter/ota"


def ssh_base(args) -> list[str]:
    base = ["ssh", "-o", "BatchMode=yes"]
    if args.identity:
        base += ["-o", "IdentitiesOnly=yes", "-i", str(Path(args.identity).expanduser())]
    return base + [args.ssh]


def scp_base(args) -> list[str]:
    base = ["scp", "-q", "-o", "BatchMode=yes"]
    if args.identity:
        base += ["-o", "IdentitiesOnly=yes", "-i", str(Path(args.identity).expanduser())]
    return base


def remote(args, cmd: str) -> subprocess.CompletedProcess:
    return subprocess.run(ssh_base(args) + [cmd], text=True, capture_output=True)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--key", help="chave RSA-3072 (ou CLAUDEMETER_SIGNING_KEY)")
    ap.add_argument("--ssh", default="jimmycl@jimmylab")
    ap.add_argument("--identity", help="chave SSH (ex.: ~/.ssh/aria_pi)")
    ap.add_argument("--remote-dir", default=REMOTE_DIR)
    ap.add_argument("--dry-run", action="store_true", help="só gera arquivos locais")
    ap.add_argument("--step", choices=["all", "bin", "manifest"], default="all")
    ap.add_argument("--build-dir", default="build", help="subdiretório de firmware/ (padrão: build)")
    ap.add_argument("--allow-rollback-test", action="store_true",
                    help="permite publicar um ROLLBACK TEST BUILD (teste controlado)")
    args = ap.parse_args()
    fw.set_build_dir(args.build_dir)

    key = signing_key_arg(args.key)
    ver = version_txt()
    name = f"{PROJECT}-{ver}.bin"
    out_dir = fw.BUILD / "ota"
    signed = out_dir / name
    if args.step == "manifest":
        # Reusa o binário já enviado: mesma assinatura, mesmo hash
        if not signed.is_file():
            die(f"{signed} não existe — rode antes --step bin")
        desc = app_desc(signed)
        if desc["project"] != PROJECT or desc["version"] != ver:
            die(f"{signed.name} é {desc['project']} {desc['version']}, esperado {ver}")
        run(espsecure("verify_signature", "--version", "2", "--keyfile", key, signed))
    else:
        signed = sign_app(key, signed)
    digest = sha256(signed)

    if fw.is_rollback_test(signed):
        if not args.allow_rollback_test:
            die("imagem é um ROLLBACK TEST BUILD — use --allow-rollback-test para publicá-la")
        print("\n*** ATENÇÃO: publicando ROLLBACK TEST BUILD (reprova o autoteste) ***\n")

    manifest = {
        "project": PROJECT,
        "version": ver,
        "url": f"/firmware/{name}",
        "size": signed.stat().st_size,
        "sha256": digest,
        "released": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    manifest_path = out_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))

    if args.dry_run:
        print(f"\n--dry-run: arquivos em {out_dir}; nada enviado.")
        return

    rdir = args.remote_dir.rstrip("/")

    # Versão publicada atual — não retroceder nem republicar a mesma
    cur = remote(args, f"cat {rdir}/manifest.json 2>/dev/null")
    if cur.returncode == 0 and cur.stdout.strip():
        published = json.loads(cur.stdout)["version"]
        if semver(ver) <= semver(published):
            die(f"servidor já publica {published}; incremente firmware/version.txt")

    # Imutabilidade do binário versionado
    chk = remote(args, f"sha256sum {rdir}/{name} 2>/dev/null")
    if chk.returncode == 0 and chk.stdout.split():
        if chk.stdout.split()[0] != digest:
            die(f"{name} já existe no servidor com outro conteúdo")
        print(f"{name} já está no servidor com o mesmo hash")
    elif args.step == "manifest":
        die(f"{name} não está no servidor — rode antes --step bin")
    else:
        run(ssh_base(args) + [f"mkdir -p {rdir}"])
        run(scp_base(args) + [signed, f"{args.ssh}:{rdir}/.{name}.tmp"])
        run(ssh_base(args) + [f"chmod 644 {rdir}/.{name}.tmp && mv {rdir}/.{name}.tmp {rdir}/{name}"])

    if args.step == "bin":
        print(f"\n.bin enviado ({signed.stat().st_size} B, sha256 {digest[:16]}…); "
              "manifest NÃO publicado — valide e rode --step manifest")
        return

    run(scp_base(args) + [manifest_path, f"{args.ssh}:{rdir}/.manifest.json.tmp"])
    run(ssh_base(args) + [f"chmod 644 {rdir}/.manifest.json.tmp && "
                          f"mv {rdir}/.manifest.json.tmp {rdir}/manifest.json"])
    print(f"\nPublicado: {PROJECT} {ver} ({manifest['size']} B, sha256 {digest[:16]}…)")


if __name__ == "__main__":
    main()
