"""Gera a página compartilhável (coentro-teste4.html) com um retrato atual dos dados.

Lê o Apps Script de LEITURA (não altera a planilha), injeta o JSON no modelo.html
e grava coentro-teste4.html na mesma pasta. Depois é só republicar esse arquivo.
"""
import json
import pathlib
import urllib.request

URL = "https://script.google.com/macros/s/AKfycbz91Md32VDhHALkvZaZhUppX8VS2xNAbECZWS0euMBIwfX9wXWbSXSjkxqIcWahipbZKQ/exec"
PASTA = pathlib.Path(__file__).parent

with urllib.request.urlopen(URL, timeout=180) as r:
    dados = json.loads(r.read().decode("utf-8"))
if "erro" in dados:
    raise SystemExit("O script respondeu com erro: " + dados["erro"])

modelo = (PASTA / "modelo.html").read_text(encoding="utf-8")
marcador = "/*__DADOS__*/null"
assert marcador in modelo, "marcador de dados não encontrado no modelo"
# "</" escapado para o JSON nunca fechar a tag <script> por engano
js = json.dumps(dados, separators=(",", ":"), ensure_ascii=False).replace("</", "<\\/")
(PASTA / "coentro-teste4.html").write_text(modelo.replace(marcador, js), encoding="utf-8")
print("ok:", dados["nLeituras"], "leituras,", len(dados["pontos"]), "janelas,", len(dados["regas"]), "regas")
