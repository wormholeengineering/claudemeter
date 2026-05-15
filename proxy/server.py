"""
ClaudeMeter Proxy
- Fonte: claude.ai API interna via cookies de sessão

Uso:
    uvicorn server:app --host 0.0.0.0 --port 8000

Saldo extra manual:
    POST /balance {"value": 12.50}
"""

import asyncio
import logging
import os
import time
from dataclasses import dataclass, field

from fastapi import FastAPI
from fastapi.responses import JSONResponse
from pydantic import BaseModel

from claude_auth import fetch_claude_usage, inject_cookies, _load_cookies, _cookies_valid

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("claudemeter")

app = FastAPI(title="ClaudeMeter Proxy")

EXTRA_BALANCE   = float(os.environ.get("CLAUDE_EXTRA_BALANCE", "-1"))
SCRAPE_INTERVAL = int(os.environ.get("SCRAPE_INTERVAL_SEC", "300"))  # 5 min


# ── Cache do último scrape ────────────────────────────────────────────────────

@dataclass
class Cache:
    data: dict = field(default_factory=dict)
    fetched_at: float = 0.0


_cache = Cache()
_scrape_lock = asyncio.Lock()
_manual_balance = EXTRA_BALANCE


# ── Endpoint principal ────────────────────────────────────────────────────────

@app.get("/usage")
async def get_usage() -> JSONResponse:
    global _cache, _manual_balance
    now = time.monotonic()

    if _cache.data and (now - _cache.fetched_at) < SCRAPE_INTERVAL:
        age = int(now - _cache.fetched_at)
        return JSONResponse({**_cache.data, "cache_age_secs": age})

    async with _scrape_lock:
        if _cache.data and (time.monotonic() - _cache.fetched_at) < SCRAPE_INTERVAL:
            age = int(time.monotonic() - _cache.fetched_at)
            return JSONResponse({**_cache.data, "cache_age_secs": age})

        log.info("Consultando claude.ai...")
        usage = await fetch_claude_usage()

        api_balance = usage.pop("extra_balance", -1.0)
        effective_balance = api_balance if api_balance >= 0 else _manual_balance
        usage["tokens_reset_secs"] = usage.pop("reset_secs", -1)

        _cache.data = {k: v for k, v in usage.items()}
        _cache.data["extra_balance"] = effective_balance
        _cache.fetched_at = time.monotonic()

    result = {**_cache.data, "cache_age_secs": int(time.monotonic() - _cache.fetched_at)}
    if _manual_balance >= 0 and _cache.data.get("extra_balance", -1) < 0:
        result["extra_balance"] = _manual_balance
    return JSONResponse(result)


# ── Endpoints auxiliares ──────────────────────────────────────────────────────

class BalanceUpdate(BaseModel):
    value: float

@app.post("/balance")
async def set_balance(body: BalanceUpdate) -> dict:
    """Atualiza saldo extra manualmente."""
    global _manual_balance
    _manual_balance = body.value
    log.info("Saldo extra atualizado: %.2f", _manual_balance)
    return {"ok": True, "extra_balance": _manual_balance}


@app.post("/refresh")
async def force_refresh() -> dict:
    """Força um novo scrape imediatamente (ignora cache)."""
    global _cache
    _cache.fetched_at = 0.0
    return {"ok": True, "message": "Cache invalidado — próxima chamada fará scrape"}


@app.post("/cookies")
async def upload_cookies(cookies: list[dict]) -> dict:
    """
    Recebe cookies exportados do browser (extensão Cookie-Editor) e os salva.

    Como usar:
      1. Instale a extensão "Cookie-Editor" no Chrome/Firefox
      2. Acesse claude.ai (já logado)
      3. Abra a extensão → Export → Export as JSON → copie o conteúdo
      4. Cole no comando abaixo:

         curl -X POST http://192.168.68.108:8000/cookies \\
              -H "Content-Type: application/json" \\
              -d '<JSON_COPIADO>'
    """
    ok = inject_cookies(cookies)
    global _cache
    _cache.fetched_at = 0.0
    return {
        "ok": ok,
        "cookies_count": len(cookies),
        "message": "Cookies salvos. Próxima chamada a /usage fará scrape com a nova sessão.",
    }


@app.get("/cookies/status")
async def cookies_status() -> dict:
    """Informa se há cookies salvos e válidos."""
    cookies = _load_cookies()
    if not cookies:
        return {"has_cookies": False, "valid": False, "message": "Nenhum cookie salvo"}
    valid = _cookies_valid(cookies)
    names = [c.get("name") for c in cookies]
    return {
        "has_cookies": True,
        "valid": valid,
        "count": len(cookies),
        "session_keys_found": [n for n in names if "session" in n.lower() or n == "sessionKey"],
        "message": "Sessão OK" if valid else "Cookie de sessão não encontrado — verifique o export",
    }


@app.get("/health")
async def health() -> dict:
    age = int(time.monotonic() - _cache.fetched_at) if _cache.data else -1
    return {"ok": True, "cache_age_secs": age, "has_data": bool(_cache.data)}
