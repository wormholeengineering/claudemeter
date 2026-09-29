FROM python:3.13-slim

# curl_cffi precisa de libcurl e dependências de compilação
RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY server.py claude_auth.py admin.py ./

# Roda sem root: UID/GID vêm do compose (user: PUID:PGID, dono de ./data)
VOLUME ["/data"]
ENV DATA_DIR=/data

EXPOSE 8000
# --no-proxy-headers: request.client é sempre o peer TCP real (loopback = admin)
CMD ["uvicorn", "server:app", "--host", "0.0.0.0", "--port", "8000", "--no-proxy-headers"]
