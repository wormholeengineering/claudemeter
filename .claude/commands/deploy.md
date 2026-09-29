# ClaudeMeter — Deploy

Guia de referência rápida para build, flash, deploy no Pi e push ao GitHub.

## Hardware

| Item | Valor |
|------|-------|
| ESP32 | C3 Super Mini, porta **COM12** |
| Display | ST7735 1.8" GREENTAB, 128×160, SPI |
| Servidor | `jimmylab` — Raspberry Pi 5, `/docker/apps/claude-meter` |
| Proxy | `https://watch.jimmylab.com.br` → tunnel jimmylab → `claude-meter:8000` (rede `proxy`) |
| Repo | `https://github.com/wormholeengineering/claudemeter` |

---

## 1. Flash do firmware (ESP32)

### Build + flash em um comando

```powershell
$IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.5.3"
$PYTHON   = "C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
$PROJECT  = "C:\Users\Jimmy\OneDrive\Projetos\claudemeter\firmware"
$PORT     = "COM12"

$env:PATH = @(
    "C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin",
    "C:\Espressif\tools\cmake\3.30.2\bin",
    "C:\Espressif\tools\ninja\1.12.1",
    "C:\Espressif\tools\idf-git\2.39.2\cmd",
    "C:\Espressif\python_env\idf5.5_py3.11_env\Scripts",
    "$IDF_PATH\tools",
    $env:PATH
) -join ";"
$env:IDF_PATH            = $IDF_PATH
$env:IDF_TOOLS_PATH      = "C:\Espressif"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\python_env\idf5.5_py3.11_env"

# Só build:
& $PYTHON "$IDF_PATH\tools\idf.py" --project-dir $PROJECT build

# Build + flash:
& $PYTHON "$IDF_PATH\tools\idf.py" --project-dir $PROJECT -p $PORT flash
```

### Secrets do firmware (`firmware/main/secrets.h`, fora do Git)

```powershell
Copy-Item firmware\main\secrets.h.example firmware\main\secrets.h
```

`WIFI_SSID`, `WIFI_PASS`, `CF_ACCESS_CLIENT_ID`, `CF_ACCESS_CLIENT_SECRET`
(Service Token do Cloudflare Access; vazio = não envia os headers).
`PROXY_URL` padrão: `https://watch.jimmylab.com.br/usage` (TLS via certificate bundle do ESP-IDF).

### Pinos ST7735

| Sinal | GPIO |
|-------|------|
| MOSI  | 6    |
| SCLK  | 4    |
| CS    | 21   |
| DC    | 1    |
| RST   | 3    |

### Partição customizada

O binário do LVGL é ~1.3 MB. O projeto usa `partitions.csv` com factory de 1.94 MB.
Se aparecer erro `app partition is too small`, verifique `firmware/partitions.csv`.

### Se o display mostrar lixo

Em LVGL v9 o byte swap é feito via:
```cpp
lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
```
**Não** use `LV_COLOR_16_SWAP` no sdkconfig (foi removido no v9).

---

## 2. Deploy do proxy (JimmyLab)

`/docker/apps/claude-meter` é um clone deste repositório; `compose.yaml` na raiz.
Sem porta publicada — acesso só pela rede Docker `proxy` (cloudflared).

```bash
cd /docker/apps/claude-meter
git pull
cp -n .env.example .env                    # primeira vez
mkdir -p data && sudo chown 1000:1000 data && chmod 700 data
docker compose up -d --build
```

### Verificar

```bash
docker compose ps                                                     # healthy?
docker logs claude-meter --tail=30
docker exec claude-meter python -c "import urllib.request as u; print(u.urlopen('http://127.0.0.1:8000/usage').read().decode())"
docker run --rm --network proxy busybox wget -qO- http://claude-meter:8000/health   # caminho do cloudflared
```

### Administração (só de dentro do container)

Endpoints administrativos (`/cookies`, `/cookies/status`, `/refresh`, `/balance`) só
aceitam loopback, ou `Authorization: Bearer $ADMIN_TOKEN` se `ADMIN_TOKEN` estiver no `.env`.
O Service Token do ESP32 **não** dá acesso a eles.

```bash
docker exec    claude-meter python admin.py status
docker exec    claude-meter python admin.py refresh
docker exec    claude-meter python admin.py balance 12.50
```

### Atualizar cookies do claude.ai (quando a sessão expirar)

1. Instale a extensão **Cookie-Editor** no Chrome
2. Acesse `claude.ai` (já logado)
3. Cookie-Editor → Export → Export as JSON → salve como `cookies.json` no Pi
4. Envie e apague o arquivo:

```bash
docker exec -i claude-meter python admin.py cookies < cookies.json && shred -u cookies.json
```

### Variáveis de ambiente (`.env`, fora do Git)

Ver `.env.example`: `TZ` (America/Porto_Velho), `SCRAPE_INTERVAL_SEC`, `CLAUDE_EXTRA_BALANCE`, `ADMIN_TOKEN`.

---

## 3. Git / GitHub

```bash
# Push normal após mudanças
git add -p                    # review interativo
git commit -m "mensagem"
git push

# Ver repo
gh repo view --web
```

### O que NÃO commitar (já no .gitignore)

- `.env` — contém segredos
- `data/`, `*cookies*.json`, `*.har` — cookies de sessão do claude.ai
- `firmware/main/secrets.h` — Wi-Fi e Service Token
- `firmware/build/` — artefatos de compilação
- `firmware/managed_components/` — dependências baixadas
- `firmware/sdkconfig` — gerado automaticamente

---

## 4. Acesso SSH ao servidor

```bash
ssh jimmylab "docker ps"
ssh jimmylab "docker logs claude-meter --tail=50"
ssh jimmylab "docker inspect --format '{{.State.Health.Status}}' claude-meter"
```
