# ClaudeMeter — Deploy

Guia de referência rápida para build, flash, deploy no Pi e push ao GitHub.

## Hardware

| Item | Valor |
|------|-------|
| ESP32 | C3 Super Mini, porta **COM12** |
| Display | ST7735 1.8" GREENTAB, 128×160, SPI |
| Pi | Raspberry Pi 5, IP `192.168.68.108`, usuário `pi` |
| Proxy | `http://192.168.68.108:8000` |
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

### Variáveis importantes no firmware (`main.cpp`)

```cpp
#define WIFI_SSID   "BRARUS_IoT"
#define PROXY_URL   "http://192.168.68.108:8000/usage"
#define REFRESH_SEC 60      // busca proxy a cada 60s
```

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

## 2. Deploy do proxy (Raspberry Pi)

### Enviar arquivos e rebuildar

```bash
scp proxy/server.py proxy/claude_auth.py proxy/requirements.txt \
    proxy/Dockerfile proxy/docker-compose.yml \
    pi:/home/pi/claudemeter/

ssh pi "cd /home/pi/claudemeter && docker compose up -d --build"
```

### Verificar se está funcionando

```bash
ssh pi "curl -s http://localhost:8000/usage | python3 -m json.tool"
ssh pi "docker logs claudemeter --tail=30"
```

### Forçar refresh dos dados

```bash
ssh pi "curl -s -X POST http://localhost:8000/refresh"
```

### Atualizar cookies do claude.ai (quando a sessão expirar)

1. Instale a extensão **Cookie-Editor** no Chrome
2. Acesse `claude.ai` (já logado)
3. Cookie-Editor → Export → Export as JSON → copie
4. Cole no comando:

```bash
curl -X POST http://192.168.68.108:8000/cookies \
     -H "Content-Type: application/json" \
     -d '<JSON_COPIADO>'
```

### Variáveis de ambiente (`.env` no Pi)

```bash
# Opcional — intervalo entre scrapes (padrão 300s)
SCRAPE_INTERVAL_SEC=300

# Opcional — saldo extra manual (se não vier dos cookies)
# CLAUDE_EXTRA_BALANCE=12.50
```

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

- `proxy/.env` — contém segredos
- `*.har` — contém cookies de sessão
- `firmware/build/` — artefatos de compilação
- `firmware/managed_components/` — dependências baixadas
- `firmware/sdkconfig` — gerado automaticamente

---

## 4. Acesso SSH ao Pi

```bash
ssh pi "docker ps"
ssh pi "docker logs claudemeter --tail=50"
ssh pi "curl -s http://localhost:8000/health"
```
