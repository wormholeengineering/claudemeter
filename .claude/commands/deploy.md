# ClaudeMeter — Deploy

Guia de referência rápida: build, gravação física, FOTA, servidor e Git.

## Visão geral

| Item | Valor |
|------|-------|
| ESP32 | C3 Super Mini, porta **COM12** |
| Display | ST7735 1.8" GREENTAB, 128×160, SPI |
| Servidor | `jimmylab` — Raspberry Pi 5, `/docker/apps/claude-meter` (SSH `jimmycl@jimmylab`, chave `~/.ssh/aria_pi`) |
| Público | `https://watch.jimmylab.com.br` → tunnel jimmylab → `watch:80` (nginx, rede `proxy`) → `claude-meter:8000` (rede interna) |
| Endpoints | `GET /usage`, `GET /health`, `GET /firmware/manifest.json`, `GET /firmware/claudemeter-X.Y.Z.bin` — o resto é 404 |
| Repo | `https://github.com/wormholeengineering/claudemeter` |

---

## 1. Firmware

### Ambiente ESP-IDF (PowerShell)

```powershell
$IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.5.3"
$PYTHON   = "C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
$PROJECT  = "C:\Users\Jimmy\OneDrive\Projetos\claudemeter\firmware"

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

& $PYTHON "$IDF_PATH\tools\idf.py" --project-dir $PROJECT build
```

`idf.py flash` **não** serve mais: a imagem precisa ser assinada (ver 1.3).
Depois de mudar `sdkconfig.defaults`, apague `firmware/sdkconfig` e rode o build de novo.

### 1.1 Secrets (`firmware/main/secrets.h`, fora do Git)

```powershell
Copy-Item firmware\main\secrets.h.example firmware\main\secrets.h
```

| Define | Uso |
|--------|-----|
| `WIFI_NETWORKS` | Lista `{ {"ssid","senha"}, ... }` tentada em ordem |
| `RECOVERY_AP_PASS` | Senha WPA2 (8–63) da SoftAP de recuperação — anote em local seguro |
| `CF_ACCESS_CLIENT_ID` / `CF_ACCESS_CLIENT_SECRET` | Service Token do Cloudflare Access (vazio = não envia) |
| `WATCH_BASE_URL` | Opcional; padrão `https://watch.jimmylab.com.br` |

Os secrets são compilados no firmware; o `.bin` de OTA só deve ser servido
atrás do Cloudflare Access.

### 1.2 Chave de assinatura (uma vez)

Imagens são assinadas com RSA-3072 (Secure Boot V2 *sem* Secure Boot de
hardware — nenhum eFuse é queimado). O firmware em execução só aceita OTA
assinado pela **mesma chave**. Perder a chave = voltar a precisar de USB.

```powershell
& $PYTHON -m espsecure generate_signing_key --version 2 --scheme rsa3072 D:\chaves\claudemeter_signing.pem
$env:CLAUDEMETER_SIGNING_KEY = "D:\chaves\claudemeter_signing.pem"
```

- Fora do repositório (os scripts recusam chave dentro dele; `*.pem` está no `.gitignore`).
- Backup offline (ex.: pendrive + gerenciador de senhas).

### 1.3 Gravação física (USB) — tabela OTA

```powershell
& $PYTHON tools\flash_device.py --port COM12            # flash_id + plano, não grava
& $PYTHON tools\flash_device.py --port COM12 --erase-and-flash
```

1. `esptool flash_id` — **se não for 4 MB o script PARA** sem apagar nada.
   A tabela OTA (2 × 1,94 MB) não cabe em 2 MB.
2. Assina `build/claudemeter.bin`.
3. `erase_flash` (apaga a NVS) + `write_flash`: bootloader, tabela, `otadata`
   inicial e app assinado em `ota_0`.

### 1.4 Partições (`firmware/partitions.csv`, flash 4 MB)

| Nome | Tipo | Offset | Tamanho |
|------|------|--------|---------|
| nvs | data/nvs | 0x9000 | 0x4000 |
| otadata | data/ota | 0xD000 | 0x2000 |
| phy_init | data/phy | 0xF000 | 0x1000 |
| ota_0 | app | 0x10000 | 0x1F0000 |
| ota_1 | app | 0x200000 | 0x1F0000 |

### 1.5 Publicar atualização (FOTA)

1. Incremente `firmware/version.txt` (semver; o dispositivo só baixa versão **maior**).
2. Build.
3. Publique:

```powershell
& $PYTHON tools\publish_firmware.py --dry-run                     # só gera em build/ota
& $PYTHON tools\publish_firmware.py --identity ~/.ssh/aria_pi     # envia ao jimmylab
```

O script assina, verifica, calcula SHA-256, envia o `.bin` e **depois** o
`manifest.json` (ambos `.tmp` + `mv`). Recusa republicar versão ≤ publicada
e sobrescrever `.bin` existente com outro conteúdo.

O dispositivo verifica o manifest 2 min após validar o boot e depois a cada
6 h (±30 min); com bateria < 30% e sem carregar adia (até 24 h).

### 1.6 Validação e rollback

- Imagem nova boota em `PENDING_VERIFY`.
- Após 60 s, autoteste **local**: NVS grava/lê, display desenhou, stack Wi-Fi
  iniciou, tasks criadas, loop LVGL vivo → marca válida.
- Falha local → marca inválida e volta para a anterior.
- Panic / watchdog (15 s) / reset antes de validar → o bootloader volta sozinho.
- Internet, Access ou manifest indisponíveis **não** causam rollback (só log).
- Versão que sofreu rollback fica em NVS (`ota/bad_ver`) e não é baixada de novo;
  publique uma versão maior com a correção.

**Antes de fechar o equipamento**: publique 1.0.1 e confirme o OTA; depois
publique uma versão que falhe o autoteste e confirme o rollback.

### 1.7 Recuperação de Wi-Fi

Após 15 min sem conectar a **nenhuma** rede, o display mostra
`RECUPERACAO Wi-Fi` e abre a rede `ClaudeMeter-XXXX` (senha
`RECOVERY_AP_PASS`). Acesse `http://192.168.4.1`, informe a nova rede e o
aparelho reinicia. Sem configuração em 10 min, reinicia e tenta de novo.

Troca planejada de senha: OTA com rede antiga + nova em `WIFI_NETWORKS` →
trocar no roteador → OTA removendo a antiga.

### Pinos ST7735 (`main.cpp`)

| Sinal | GPIO |
|-------|------|
| MOSI  | 1    |
| SCLK  | 21   |
| CS    | 4    |
| DC    | 3    |
| RST   | 2    |
| ADC bateria | 0 |

### Se o display mostrar lixo

Em LVGL v9 o byte swap é feito via:
```cpp
lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
```
**Não** use `LV_COLOR_16_SWAP` no sdkconfig (foi removido no v9).

---

## 2. Servidor (JimmyLab)

`/docker/apps/claude-meter` é um clone deste repositório; `compose.yaml` na raiz.
Nenhuma porta publicada. Só `watch` (nginx) fica na rede `proxy`; a API
`claude-meter` fica numa rede interna.

```bash
cd /docker/apps/claude-meter
git pull
cp -n .env.example .env                              # primeira vez (PUID/PGID=1001)
mkdir -p data ota && chmod 700 data                  # dono jimmycl (1001)
docker compose up -d --build
```

```
/docker/apps/claude-meter/
├── compose.yaml, watch/nginx.conf, proxy/   (Git)
├── .env                                     (fora do Git)
├── data/claude_cookies.json                 (fora do Git — secret)
└── ota/manifest.json, claudemeter-X.Y.Z.bin (fora do Git — publish_firmware.py)
```

### Verificar

```bash
docker compose ps                                                         # healthy?
docker logs claude-meter --tail=30
docker run --rm --network proxy busybox wget -qO- http://watch/health     # mesmo caminho do cloudflared
docker run --rm --network proxy busybox wget -qO- http://watch/firmware/manifest.json
```

### Administração (só de dentro do container)

`/cookies`, `/cookies/status`, `/refresh` e `/balance` não passam pelo nginx
(404) e, na API, só aceitam loopback ou `Authorization: Bearer $ADMIN_TOKEN`.

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

Ver `.env.example`: `TZ` (America/Porto_Velho), `PUID`/`PGID`, `SCRAPE_INTERVAL_SEC`,
`CLAUDE_EXTRA_BALANCE`, `ADMIN_TOKEN`.

---

## 3. Git / GitHub

### O que NÃO commitar (já no .gitignore)

- `.env` — contém segredos
- `data/`, `*cookies*.json`, `*.har` — cookies de sessão do claude.ai
- `firmware/main/secrets.h` — Wi-Fi, senha de recuperação e Service Token
- `*.pem`, `*signing*key*` — chave de assinatura do firmware
- `ota/` — binários publicados
- `firmware/build/`, `firmware/managed_components/`, `firmware/sdkconfig`

---

## 4. Acesso SSH ao servidor

```bash
ssh -i ~/.ssh/aria_pi jimmycl@jimmylab "docker ps"
ssh -i ~/.ssh/aria_pi jimmycl@jimmylab "docker logs watch --tail=50"
ssh -i ~/.ssh/aria_pi jimmycl@jimmylab "docker inspect --format '{{.State.Health.Status}}' claude-meter"
```
