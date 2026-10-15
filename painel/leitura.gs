/**
 * SIS — Painel do Teste 4 (SOMENTE LEITURA)
 *
 * Projeto Apps Script SEPARADO do que a ESP usa. Ele só LÊ as abas "Dia N - ..."
 * da planilha do Teste 4 e devolve um resumo (janelas de 5 min) pro painel.
 * Não existe nenhuma chamada de escrita aqui, e o appsscript.json restringe a
 * permissão a "spreadsheets.readonly": mesmo que alguém tentasse, o Google
 * bloquearia qualquer alteração na planilha.
 */
var PLANILHA_ID = '1JbIFGUymFJTwUAq3RjX3L5ghtq79F0x4078eHhsPFHA';
var BALDE_MS = 5 * 60 * 1000;            // agrega em janelas de 5 min
var MIN_VALIDO = 300, MAX_VALIDO = 2600; // mesma faixa válida do firmware
var FOLGA_REGA_MS = 15 * 60 * 1000;      // leituras ABERTA separadas por mais que isso = regas diferentes

function doGet(e) {
  var cb = (e && e.parameter && e.parameter.callback) || '';
  var dados;
  try {
    dados = montarResumo();
  } catch (err) {
    dados = { erro: String(err) };
  }
  var json = JSON.stringify(dados);
  // JSONP: o painel carrega como <script>, então funciona até abrindo o HTML direto do PC.
  if (/^[A-Za-z_$][\w$]*$/.test(cb)) {
    return ContentService.createTextOutput(cb + '(' + json + ');')
      .setMimeType(ContentService.MimeType.JAVASCRIPT);
  }
  return ContentService.createTextOutput(json)
    .setMimeType(ContentService.MimeType.JSON);
}

function valido(v) {
  return (typeof v === 'number' && v >= MIN_VALIDO && v <= MAX_VALIDO) ? v : null;
}

function mediana(a) {
  if (!a.length) return null;
  a.sort(function (x, y) { return x - y; });
  var m = a.length >> 1;
  return a.length % 2 ? a[m] : (a[m - 1] + a[m]) / 2;
}

// Resume UMA aba: janelas de 5 min + trechos de rega + última leitura.
function resumirAba(sh, n, fase) {
  var v = sh.getRange(2, 1, n - 1, 6).getValues();
  var linhas = []; // [t, s1, s2, aberta, ciclo]
  for (var i = 0; i < v.length; i++) {
    var r = v[i];
    if (!(r[0] instanceof Date)) continue; // pula linhas de teste tipo "CHK-T3"
    linhas.push([r[0].getTime(), valido(r[1]), valido(r[2]), r[4] === 'ABERTA' ? 1 : 0,
                 typeof r[5] === 'number' ? r[5] : null]);
  }
  linhas.sort(function (a, b) { return a[0] - b[0]; });

  // Regas = trechos contínuos com válvula ABERTA
  var regas = [], ini = null, fim = null, cic = null;
  linhas.forEach(function (l) {
    if (l[3]) {
      if (ini !== null && l[0] - fim > FOLGA_REGA_MS) { regas.push([ini, fim, cic]); ini = null; }
      if (ini === null) { ini = l[0]; cic = l[4]; }
      fim = l[0];
    } else if (ini !== null) {
      regas.push([ini, fim, cic]);
      ini = null;
    }
  });
  if (ini !== null) regas.push([ini, fim, cic]);

  // Janelas de 5 min: mediana de cada sensor (robusta a picos) + fração do tempo com válvula aberta
  var baldes = {}, ordem = [];
  linhas.forEach(function (l) {
    var k = Math.floor(l[0] / BALDE_MS) * BALDE_MS;
    var b = baldes[k];
    if (!b) { b = baldes[k] = { s1: [], s2: [], ab: 0, n: 0 }; ordem.push(k); }
    if (l[1] !== null) b.s1.push(l[1]);
    if (l[2] !== null) b.s2.push(l[2]);
    b.ab += l[3];
    b.n++;
  });
  var pontos = ordem.map(function (k) {
    var b = baldes[k];
    var m1 = mediana(b.s1), m2 = mediana(b.s2);
    return [k, m1 === null ? null : Math.round(m1), m2 === null ? null : Math.round(m2),
            Math.round(100 * b.ab / b.n) / 100, fase];
  });

  var u = linhas.length ? linhas[linhas.length - 1] : null;
  return { p: pontos, r: regas, n: linhas.length,
           u: u ? { t: u[0], s1: u[1], s2: u[2], aberta: u[3], ciclo: u[4], fase: fase } : null };
}

function montarResumo() {
  var ss = SpreadsheetApp.openById(PLANILHA_ID);
  // Abas de dias que já fecharam não mudam: o resumo delas fica guardado (até 6 h).
  // A chave inclui o nº de linhas, então a aba do dia atual (que cresce) é sempre relida.
  var cache = CacheService.getScriptCache();
  var abas = [], pontos = [], regas = [], nLeituras = 0, ultima = null;

  ss.getSheets().forEach(function (sh) {
    var nome = sh.getName();
    if (!/^Dia /.test(nome)) return;
    abas.push(nome);
    var n = sh.getLastRow();
    if (n < 2) return;
    var fase = /com cultura/i.test(nome) ? 1 : 0;
    var chave = Utilities.base64EncodeWebSafe(Utilities.computeDigest(
      Utilities.DigestAlgorithm.MD5, 'v1|' + sh.getSheetId() + '|' + nome + '|' + n));
    var guardado = cache.get(chave);
    var res = guardado ? JSON.parse(guardado) : resumirAba(sh, n, fase);
    if (!guardado) {
      try { cache.put(chave, JSON.stringify(res), 21600); } catch (e) { /* resumo grande demais p/ cache: só relê */ }
    }
    pontos = pontos.concat(res.p);
    regas = regas.concat(res.r);
    nLeituras += res.n;
    if (res.u && (!ultima || res.u.t > ultima.t)) ultima = res.u;
  });

  // junta as abas: janelas na ordem do tempo (uma janela por horário) e regas que cruzam a virada de aba
  pontos.sort(function (a, b) { return a[0] - b[0]; });
  pontos = pontos.filter(function (p, i) { return i === 0 || p[0] !== pontos[i - 1][0]; });
  regas.sort(function (a, b) { return a[0] - b[0]; });
  var juntas = [];
  regas.forEach(function (r) {
    var ult = juntas[juntas.length - 1];
    if (ult && r[0] - ult[1] <= FOLGA_REGA_MS) ult[1] = Math.max(ult[1], r[1]);
    else juntas.push(r.slice());
  });

  return {
    atualizado: new Date().getTime(),
    abas: abas,
    nLeituras: nLeituras,
    pontos: pontos,
    regas: juntas,
    ultima: ultima
  };
}
