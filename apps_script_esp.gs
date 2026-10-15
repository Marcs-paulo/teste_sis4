// SIS — Apps Script da ESP (Teste 3/4): recebe os dados (doPost) e guarda a
// config remota (doGet). Substitui o código do projeto que a ESP já usa
// (a URL que está em segredos.h). Atualize a implantação EXISTENTE para não mudar a URL.
//
// Guarda teste/modo, os parâmetros de rega (limiar, maxH, minH) e a versão de
// firmware pedida (fw). Também registra a versão que a placa diz estar rodando
// (fwAtual) e quando ela falou pela última vez (visto). O firmware antigo
// ignora os campos novos, então pode atualizar o script antes da placa.
//
// O TOKEN dos comandos NÃO fica no código (o repositório é público):
// Configurações do projeto (engrenagem) -> Propriedades do script ->
// adicionar propriedade TOKEN com o valor do token.

var PLAN_T3 = "1JamvhFAXVtLCseJKsv3MaIgsxnR1S1TQzoKT3zmmRok";
var PLAN_T4 = "1JbIFGUymFJTwUAq3RjX3L5ghtq79F0x4078eHhsPFHA";

// Padrões e faixas aceitas (iguais às do firmware)
var REGA = {
  limiar: { padrao: 1500, min: 900, max: 2400 },
  maxH:   { padrao: 24,   min: 6,   max: 96 },
  minH:   { padrao: 3,    min: 1,   max: 48 }
};

function jsonOut(obj){
  return ContentService.createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}

function doPost(e) {
  try {
    var data   = JSON.parse(e.postData.contents);
    var p      = PropertiesService.getScriptProperties();
    var modo   = p.getProperty("MODO") || "observa";
    var rotulo = (modo == "cultivo") ? "com cultura" : "sem cultura";
    var teste  = data.teste || 3;
    var id     = (teste == 4) ? PLAN_T4 : PLAN_T3;
    var ss     = SpreadsheetApp.openById(id);
    var nome   = "Dia " + data.testDay + " - teste " + rotulo;
    var sheet  = ss.getSheetByName(nome);
    if (!sheet) {
      sheet = ss.insertSheet(nome);
      sheet.getRange(1,1,1,6).setValues([["Data/Hora","Sensor 1","Sensor 2","Média","Válvula","Ciclo"]]);
      sheet.getRange(1,1,1,6).setFontWeight("bold");
    }
    var rows = data.readings.map(function(r){ return [r.timestamp,r.s1,r.s2,r.avg,r.valve,r.cycle||""]; });
    sheet.getRange(sheet.getLastRow()+1,1,rows.length,6).setValues(rows);
    return jsonOut({status:"ok", rows:rows.length, teste:teste, aba:nome});
  } catch(err) {
    return jsonOut({status:"erro", msg:err.toString()});
  }
}

function lerRega(p, campo) {
  var v = Number(p.getProperty("REGA_" + campo));
  return isNaN(v) || v === 0 ? REGA[campo].padrao : v;
}

function doGet(e) {
  var p = PropertiesService.getScriptProperties();
  var avisos = [];
  if (e && e.parameter && e.parameter.cmd == "set") {
    var token = p.getProperty("TOKEN");
    if (!token || e.parameter.token != token) return jsonOut({erro:"token invalido"});
    if (e.parameter.teste) p.setProperty("TESTE", String(e.parameter.teste));
    if (e.parameter.modo)  p.setProperty("MODO",  String(e.parameter.modo));

    // parâmetros de rega: valida faixa e mínimo < máximo antes de gravar
    var novo = {};
    ["limiar", "maxH", "minH"].forEach(function(c){
      novo[c] = lerRega(p, c);
      if (e.parameter[c] === undefined) return;
      var v = Math.round(Number(e.parameter[c]));
      if (isNaN(v) || v < REGA[c].min || v > REGA[c].max) {
        avisos.push(c + " precisa ficar entre " + REGA[c].min + " e " + REGA[c].max + " (nao mudou)");
      } else {
        novo[c] = v;
      }
    });
    if (novo.minH >= novo.maxH) {
      avisos.push("minH precisa ser menor que maxH (tempos nao mudaram)");
      novo.minH = lerRega(p, "minH"); novo.maxH = lerRega(p, "maxH");
    }
    p.setProperty("REGA_limiar", String(novo.limiar));
    p.setProperty("REGA_maxH",   String(novo.maxH));
    p.setProperty("REGA_minH",   String(novo.minH));

    // versão de firmware a baixar pela internet (0 = nenhuma)
    if (e.parameter.fw !== undefined) {
      var fw = Math.round(Number(e.parameter.fw));
      if (isNaN(fw) || fw < 0 || fw > 9999) avisos.push("fw precisa ser um numero entre 0 e 9999 (nao mudou)");
      else p.setProperty("FW", String(fw));
    }
  } else if (e && e.parameter && e.parameter.fwAtual) {
    // consulta da própria placa: anota a versão que está rodando
    p.setProperty("FW_ATUAL", String(e.parameter.fwAtual));
    p.setProperty("FW_VISTO", Utilities.formatDate(new Date(), "America/Sao_Paulo", "dd/MM HH:mm"));
  }
  var out = {
    teste:  Number(p.getProperty("TESTE") || "3"),
    modo:   p.getProperty("MODO") || "observa",
    limiar: lerRega(p, "limiar"),
    maxH:   lerRega(p, "maxH"),
    minH:   lerRega(p, "minH"),
    fw:     Number(p.getProperty("FW") || "0"),
    fwAtual: Number(p.getProperty("FW_ATUAL") || "0"),
    visto:  p.getProperty("FW_VISTO") || ""
  };
  if (avisos.length) out.avisos = avisos;
  return jsonOut(out);
}

function removerDuplicadas() {
  [PLAN_T3, PLAN_T4].forEach(function(id){
    var ss = SpreadsheetApp.openById(id);
    ss.getSheets().forEach(function(sheet) {
      if (!/^Dia /.test(sheet.getName())) return;
      var dados = sheet.getDataRange().getValues();
      if (dados.length < 2) return;
      var vistos = {}, limpas = [dados[0]];
      for (var i = 1; i < dados.length; i++) {
        var ts = dados[i][0];
        var chave = ts instanceof Date ? ts.getTime() : ts;
        if (!vistos[chave]) { vistos[chave] = true; limpas.push(dados[i]); }
      }
      sheet.clearContents();
      sheet.getRange(1,1,limpas.length,limpas[0].length).setValues(limpas);
    });
  });
}
