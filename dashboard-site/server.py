# ApoiaSP - servidor local do dashboard (so biblioteca padrao do Python)
# Serve o index.html e repassa /api/estado para o Orion, evitando o erro de CORS.
# Uso:  python server.py   ->   abra http://localhost:8080
import json
import os
import urllib.error
import urllib.request
from datetime import datetime, timezone
from email.utils import parsedate_to_datetime
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

# Orion no Docker do seu PC. Se o Orion estiver em outro lugar, troque aqui
# (pode ser o endereco do tunel Cloudflare, ex.: https://xxxx.trycloudflare.com)
ORION_URL = os.environ.get("ORION_URL", "http://localhost:1026")
ENTIDADE = "ApoiaSP:poste-se-01"
TIPO = "TotemApoiaSP"
PORTA = 8080


def hora_do_orion(resposta):
    """Hora segundo o proprio Orion (cabecalho Date); se nao houver, usa a do PC."""
    try:
        hora = parsedate_to_datetime(resposta.headers["Date"])
        if hora.tzinfo is None:
            hora = hora.replace(tzinfo=timezone.utc)
        return hora
    except Exception:
        return datetime.now(timezone.utc)


def acrescentar_idade(corpo, agora):
    """Acrescenta _idade_s: segundos desde a ultima atualizacao gravada no Orion.
    Compara dateModified com a hora do Orion, entao relogio do Docker atrasado nao atrapalha."""
    try:
        dados = json.loads(corpo)
        modificado = dados["dateModified"]
        if isinstance(modificado, dict):
            modificado = modificado["value"]
        base = modificado.replace("Z", "").split(".")[0]
        quando = datetime.strptime(base, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=timezone.utc)
        dados["_idade_s"] = max(0, int((agora - quando).total_seconds()))
        return json.dumps(dados).encode()
    except Exception:
        return corpo  # sem _idade_s o dashboard so nao avisa "sem sinal"


class Handler(SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/api/estado"):
            self.estado()
        else:
            super().do_GET()

    def estado(self):
        url = f"{ORION_URL}/v2/entities/{ENTIDADE}?type={TIPO}&options=dateModified"
        try:
            with urllib.request.urlopen(url, timeout=5) as r:
                corpo, codigo, agora = r.read(), r.status, hora_do_orion(r)
            corpo = acrescentar_idade(corpo, agora)
        except urllib.error.HTTPError as e:      # ex.: 404 = ainda sem dados do totem
            corpo, codigo = e.read(), e.code
        except Exception as e:                   # Orion fora do ar / Docker desligado
            corpo, codigo = json.dumps({"error": str(e)}).encode(), 502
        self.send_response(codigo)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(corpo)


if __name__ == "__main__":
    print(f"Dashboard em http://localhost:{PORTA}  (Orion: {ORION_URL})")
    ThreadingHTTPServer(("", PORTA), Handler).serve_forever()