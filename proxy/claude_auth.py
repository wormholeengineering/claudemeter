"""
Acessa a API interna do claude.ai com os cookies de sessão.
Usa curl_cffi para impersonar o TLS fingerprint do Chrome e passar pelo Cloudflare.

Endpoints descobertos via HAR (claude.ai/settings/usage):
  GET /api/organizations/{org_id}/usage
    → {"five_hour": {"utilization": 49.0, "resets_at": "..."},
       "seven_day": {"utilization": 18.0, "resets_at": "..."},
       "seven_day_omelette": {"utilization": 0.0, "resets_at": null},
       "extra_usage": {"is_enabled": true, "used_credits": 15896.0, "currency": "BRL"}}
  GET /api/organizations/{org_id}/prepaid/credits
    → {"amount": 1892, "currency": "BRL", "auto_reload_settings": null}
      (amount em centavos de BRL: 1892 = R$18.92)
"""

import json
import logging
import os
from datetime import datetime, timezone
from pathlib import Path

from curl_cffi.requests import AsyncSession as CurlSession

log = logging.getLogger("claudemeter.auth")

COOKIES_FILE = Path(os.environ.get("DATA_DIR", "/data")) / "claude_cookies.json"
COOKIES_FILE.parent.mkdir(parents=True, exist_ok=True)

# Cookies do Cloudflare que expiram rapidamente e podem causar problemas se desatualizados
_CLOUDFLARE_COOKIES = {"__cf_bm", "_cfuvid", "cf_clearance"}

BROWSER_HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
    ),
    "Accept": "application/json, text/plain, */*",
    "Accept-Language": "pt-BR,pt;q=0.9,en;q=0.8",
    "Accept-Encoding": "gzip, deflate, br",
    "Referer": "https://claude.ai/settings/usage",
    "Origin": "https://claude.ai",
    "sec-ch-ua": '"Chromium";v="124", "Google Chrome";v="124", "Not-A.Brand";v="99"',
    "sec-ch-ua-mobile": "?0",
    "sec-ch-ua-platform": '"Windows"',
    "sec-fetch-dest": "empty",
    "sec-fetch-mode": "cors",
    "sec-fetch-site": "same-origin",
}


# ── Gestão de cookies ─────────────────────────────────────────────────────────

def _save_cookies(cookies: list[dict]) -> None:
    COOKIES_FILE.write_text(json.dumps(cookies, indent=2))
    log.info("Cookies salvos em %s", COOKIES_FILE)


def _load_cookies() -> list[dict] | None:
    if COOKIES_FILE.exists():
        try:
            return json.loads(COOKIES_FILE.read_text())
        except Exception:
            pass
    return None


def _cookies_valid(cookies: list[dict]) -> bool:
    session_keys = {"sessionKey", "__Secure-next-auth.session-token", "activitySessionId"}
    return any(c.get("name") in session_keys for c in cookies)


def _to_curl_cookies(cookies: list[dict]) -> dict[str, str]:
    """
    Converte lista de cookies (formato Cookie-Editor) para dict.
    Exclui cookies Cloudflare de curta duração — curl_cffi com impersonation
    consegue passar pelo Cloudflare sem eles.
    """
    return {
        c["name"]: c["value"]
        for c in cookies
        if c.get("value") and c.get("name") not in _CLOUDFLARE_COOKIES
    }


def inject_cookies(cookies: list[dict]) -> bool:
    _save_cookies(cookies)
    return True


# ── Parser direto para endpoints conhecidos ───────────────────────────────────

def _parse_usage_response(data: dict) -> dict | None:
    """
    Parseia a resposta de /api/organizations/{org_id}/usage com estrutura exata.
    Campos: five_hour, seven_day, seven_day_omelette, extra_usage
    """
    result: dict = {}

    five_hour = data.get("five_hour")
    if isinstance(five_hour, dict) and "utilization" in five_hour:
        result["session_pct"] = float(five_hour["utilization"])
        resets_at = five_hour.get("resets_at")
        if resets_at:
            try:
                dt = datetime.fromisoformat(resets_at.replace("Z", "+00:00"))
                result["reset_secs"] = max(0, int((dt - datetime.now(timezone.utc)).total_seconds()))
            except Exception:
                pass

    seven_day = data.get("seven_day")
    if isinstance(seven_day, dict) and "utilization" in seven_day:
        result["weekly_pct"] = float(seven_day["utilization"])

    seven_day_omelette = data.get("seven_day_omelette")
    if isinstance(seven_day_omelette, dict) and "utilization" in seven_day_omelette:
        result["opus_pct"] = float(seven_day_omelette["utilization"])

    extra_usage = data.get("extra_usage")
    if isinstance(extra_usage, dict) and extra_usage.get("is_enabled"):
        used = extra_usage.get("used_credits", -1.0)
        if used >= 0:
            result["extra_used_credits"] = float(used)

    return result if result else None


def _parse_credits_response(data: dict) -> float | None:
    """
    Parseia /api/organizations/{org_id}/prepaid/credits.
    amount está em centavos de BRL: 1892 → R$18.92
    """
    amount = data.get("amount")
    if isinstance(amount, (int, float)) and amount >= 0:
        return round(float(amount) / 100, 2)
    return None


# ── Requisição a um endpoint ──────────────────────────────────────────────────

async def _get_json(client: CurlSession, url: str, label: str) -> dict | list | None:
    try:
        resp = await client.get(url, headers=BROWSER_HEADERS, allow_redirects=True)
        log.info("[%s] %s → %d", label, url.split("?")[0], resp.status_code)

        if resp.status_code != 200:
            if resp.status_code in (401, 403):
                log.warning("[%s] Sem autorização — cookies expirados?", label)
            return None

        data = resp.json()

        if isinstance(data, dict):
            log.debug("[%s] keys: %s", label, list(data.keys()))

        return data
    except Exception as e:
        log.warning("[%s] Erro: %s", label, e)
        return None


# ── Ponto de entrada público ──────────────────────────────────────────────────

async def fetch_claude_usage() -> dict:
    """
    Chama a API interna do claude.ai com os cookies de sessão.
    Retorna: session_pct, weekly_pct, opus_pct, extra_balance, reset_secs
    """
    empty = {
        "session_pct": -1.0,
        "weekly_pct": -1.0,
        "opus_pct": -1.0,
        "extra_balance": -1.0,
        "reset_secs": -1,
    }

    cookies_list = _load_cookies()
    if not cookies_list:
        log.warning("Nenhum cookie. Use POST /cookies.")
        return {**empty, "source": "no-cookies"}

    if not _cookies_valid(cookies_list):
        log.warning("Cookies sem token de sessão.")
        return {**empty, "source": "invalid-cookies"}

    jar = _to_curl_cookies(cookies_list)

    org_id = next(
        (c["value"] for c in cookies_list if c["name"] == "lastActiveOrg"), None
    )
    log.info("org_id: %s", org_id)

    if not org_id:
        log.warning("Cookie 'lastActiveOrg' não encontrado — org_id desconhecido.")
        return {**empty, "source": "no-org-id"}

    async with CurlSession(impersonate="chrome124", cookies=jar, timeout=15) as client:
        result = {**empty}

        # Endpoint primário: dados de uso da organização
        usage_data = await _get_json(
            client,
            f"https://claude.ai/api/organizations/{org_id}/usage",
            "org-usage",
        )
        if usage_data:
            parsed = _parse_usage_response(usage_data)
            if parsed:
                result.update(parsed)
                log.info("Dados de uso: %s", parsed)
            else:
                log.warning("[org-usage] Resposta inesperada: %s", list(usage_data.keys()))

        # Endpoint secundário: saldo de créditos pré-pagos
        credits_data = await _get_json(
            client,
            f"https://claude.ai/api/organizations/{org_id}/prepaid/credits",
            "prepaid-credits",
        )
        if credits_data:
            balance = _parse_credits_response(credits_data)
            if balance is not None:
                result["extra_balance"] = balance
                log.info("Saldo pré-pago: R$%.2f", balance)

        # Verifica se obteve dados úteis
        if result["session_pct"] >= 0 or result["weekly_pct"] >= 0:
            result["source"] = "claude-api"
            return result

        # Fallback: tenta bootstrap para diagnóstico
        bootstrap_data = await _get_json(
            client,
            f"https://claude.ai/edge-api/bootstrap/{org_id}/app_start"
            "?statsig_hashing_algorithm=djb2&growthbook_format=sdk"
            "&include_system_prompts=false",
            "bootstrap",
        )
        if bootstrap_data:
            log.info("[bootstrap] keys: %s", list(bootstrap_data.keys()) if isinstance(bootstrap_data, dict) else "list")

    log.warning("Nenhum endpoint retornou dados de uso. Verifique cookies.")
    return {**empty, "source": "no-data-found"}
