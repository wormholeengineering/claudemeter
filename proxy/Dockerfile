FROM python:3.13-slim

# curl_cffi precisa de libcurl e dependências de compilação
RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 \
    && rm -rf /var/lib/apt/lists/*

# Usuário sem privilégios; UID/GID 1000 = dono de ./data no host
RUN groupadd -g 1000 app && useradd -u 1000 -g 1000 -M -s /usr/sbin/nologin app

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY server.py claude_auth.py admin.py ./

RUN mkdir -p /data && chown app:app /data
VOLUME ["/data"]
ENV DATA_DIR=/data

USER app

EXPOSE 8000
# --no-proxy-headers: request.client é sempre o peer TCP real (loopback = admin)
CMD ["uvicorn", "server:app", "--host", "0.0.0.0", "--port", "8000", "--no-proxy-headers"]
