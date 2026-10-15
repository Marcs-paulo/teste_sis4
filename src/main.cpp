#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <NTPClient.h>
#include <WiFiUDP.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ArduinoOTA.h>   // OTA local (mesma rede WiFi) — p/ o amigo aí no local
#include <HTTPUpdate.h>   // atualização pela INTERNET (baixa o firmware do GitHub)
#include <esp_ota_ops.h>  // volta para a versão anterior se a nova der problema

// ── SEGREDOS (WiFi, URL do Apps Script, senha do OTA local) ──
// Ficam FORA do GitHub: copie include/segredos.exemplo.h para
// include/segredos.h e preencha. Ao ligar, a placa grava esses valores na
// memória (NVS). O firmware "público" (env release, o que vai para o GitHub e
// é baixado pela internet) não leva segredos: usa o que já está salvo na placa.
#ifndef BUILD_PUBLICO
  #if __has_include("segredos.h")
    #include "segredos.h"
  #else
    #error "Falta include/segredos.h: copie include/segredos.exemplo.h para include/segredos.h e preencha (peca os dados ao dono do projeto)."
  #endif
#endif

// Uma única URL do Apps Script serve os dois testes:
//  - POST  (enviar dados): o script grava na planilha do teste ATIVO (3 ou 4)
//  - GET   (config)      : o firmware pergunta teste/modo/rega/versão e o dono
//                          muda isso a distância abrindo a URL no navegador.
String wifiSsid, wifiSenha, scriptUrl, otaSenha;

// ── OTA local (para quem está na mesma rede da placa) ──────
#define OTA_HOSTNAME  "sis-teste3"

// ── VERSÃO E ATUALIZAÇÃO PELA INTERNET ─────────────────────
// Para publicar uma versão nova: aumente FW_VERSAO, compile o env "release" e
// suba .pio/build/release/firmware.bin numa release do GitHub com a tag
// fw-<FW_VERSAO>. Depois mande fw=<FW_VERSAO> pela config remota.
#define FW_VERSAO    2
#define FW_URL_BASE  "https://github.com/Marcs-paulo/teste_sis4/releases/download/fw-"
// Versão nova fica "em prova": se não falar com o Apps Script depois de 10 min
// ligada, ou se reiniciar 3 vezes seguidas, a placa volta sozinha para a anterior.
#define OTA_PROVA_CONFIRMA_MS (10UL * 60000UL)
#define OTA_PROVA_LIMITE_MS   (20UL * 60000UL)
#define OTA_PROVA_MAX_BOOTS   3
#define OTA_MAX_TENTATIVAS    3      // downloads que falharam antes de desistir (até reiniciar)

#define VALVE_PIN  18   // Relé da eletroválvula
#define S1_PWR     2    // VCC Sensor 1
#define S2_PWR     3    // VCC Sensor 2
#define SIGNAL1    0    // Sinal analógico Sensor 1 (ADC)
#define SIGNAL2    1    // Sinal analógico Sensor 2 (ADC)
#define BTN_PIN    19   // Botão de início (GPIO19 → GND)

// AGORA os DOIS sensores são bons e estão RASOS (~5 cm), mas em POSIÇÕES
// DIFERENTES (o defeituoso foi substituído):
//   S2 (GPIO1) = NO PONTO DA PLANTA (zona radicular da futura alface)
//   S1 (GPIO0) = LONGE, em SOLO NU (referência de evaporação p/ comparação)
// Como medem lugares diferentes, NÃO se faz média deles. Os dois são
// gravados separados (colunas S1/S2) pra análise; o CONTROLE de irrigação
// usa só o sensor do ponto da planta (SENSOR_CONTROLE).
#define USAR_S1  true   // registra (solo nu, referência)
#define USAR_S2  true   // registra (ponto da planta)
#define SENSOR_CONTROLE 2   // qual sensor rege a irrigação: 2 = S2 (planta)

#define RELE_ATIVO_EM_LOW true

// Intervalos
#define READ_INTERVAL_MS  10000UL
#define DIAG_INTERVAL_MS  2000UL
#define SEND_INTERVAL_MS  300000UL                   // envio a cada 5 min
#define PRIMEIRO_ENVIO_MS 15000UL
#define CONFIG_INTERVAL_MS 300000UL                  // consulta config remota a cada 5 min
#define WIFI_TIMEOUT_MS   20000UL
#define WIFI_CHECK_MS     10000UL
#define ENVIO_LOTE_MAX    100

// ── MODOS DE OPERAÇÃO (escolhidos A DISTÂNCIA pela config) ──
//  OBSERVA  (Teste 3, SEM cultura): satura UMA vez ao iniciar e depois só
//           observa a secagem — NÃO rega mais. Mede evaporação pura do solo
//           raso, igual ao Teste 1. É a linha de base (controle).
//  CULTIVO  (Teste 4, COM cultura): irriga de verdade — rega quando o solo
//           raso seca até o limiar (controle por umidade), mantendo a planta.
//           A diferença T4-T3 isola a transpiração da alface.
#define MODO_OBSERVA  0
#define MODO_CULTIVO  1

// Gatilho de rega no modo CULTIVO (valores do sensor raso, do Teste 3 real:
// úmido ~1000, seco 1500-2000). Rega quando a umidade de controle passa do
// limiar de forma sustentada, respeitando um mínimo entre regas.
// Limiar e tempos são AJUSTÁVEIS A DISTÂNCIA pela config remota (limiar,
// maxH, minH). Os valores abaixo são o padrão, usado até a config mandar
// outro; o último valor recebido fica salvo e sobrevive a reinícios.
#define REGA_LIMIAR_PADRAO  1500               // solo raso "seco o suficiente"
#define REGA_MAX_H_PADRAO   24                 // no máximo 24 h sem regar (garante água)
#define REGA_MIN_H_PADRAO   3                  // no mínimo 3 h entre regas (anti-encharcamento)
#define REGA_CONSEC         60                 // 60 leituras de 10 s = 10 min sustentados
// Faixas aceitas da config remota (valor fora disso é ignorado)
#define LIMIAR_MIN  900
#define LIMIAR_MAX  2400
#define MAX_H_MIN   6
#define MAX_H_MAX   96
#define MIN_H_MIN   1
#define MIN_H_MAX   48
// Se o sensor da planta falhar, não dá pra saber se o solo secou: aí a rega
// volta a ser no máximo a cada 24 h, mesmo que a config peça mais tempo.
#define REGA_MAX_SEM_SENSOR_H 24

// ── VALIDADE DO SENSOR (faixa ampliada p/ solo raso) ───────
// O Teste 3 mostrou o solo raso secando legitimamente até ~2500. Por isso a
// faixa vai até 2600 (antes 2150 cortava solo muito seco e travava a rega).
// Acima de 2600 = sensor no ar / desconectado / defeito.
#define SENSOR_MIN_VALIDO  300
#define SENSOR_MAX_VALIDO  2600

// Teto de segurança do ENCHIMENTO.
#define ENCHENDO_MAX_MS   (45UL * 60000UL)

// Platô (saturação).
#define PLATEAU_WINDOW    90
#define PLATEAU_THRESHOLD 60

#define MEDIANA_N         5
// ───────────────────────────────────────────────────────────

enum Estado { AGUARDANDO, ENCHENDO, DRENANDO, CONCLUIDO };
Estado estado    = AGUARDANDO;
int    numCiclos = 0;

// Config remota (padrão seguro: Teste 3 / OBSERVA até a config dizer o contrário)
int  modoOperacao = MODO_OBSERVA;
int  testeAtivo   = 3;
int  regaLimiar   = REGA_LIMIAR_PADRAO;
int  regaMaxH     = REGA_MAX_H_PADRAO;
int  regaMinH     = REGA_MIN_H_PADRAO;
int  fwPedido     = 0;       // versão que a config remota pediu (0 = nenhuma)
int  fwTentada    = 0;
int  fwTentativas = 0;
bool emProva      = false;   // rodando uma versão recém-baixada, ainda não confirmada

struct Leitura {
  char  timestamp[20];
  int   s1, s2;
  float avg;
  bool  valveAberta;
  int   ciclo;
};

const int BUF_MAX = 1080;
Leitura   buf[BUF_MAX];
int       bufCount = 0;

float janUmid[PLATEAU_WINDOW] = {};
int   janIdx    = 0;
int   totalLeit = 0;

int  medS1[MEDIANA_N] = {};
int  medS2[MEDIANA_N] = {};
int  medIdx   = 0;
bool medCheia = false;

int secoConsec = 0;

unsigned long tLeitura   = 0;
unsigned long tDiag      = 0;
unsigned long tEnvio     = 0;
unsigned long tConfig    = 0;
unsigned long tTeste     = 0;
unsigned long tCiclo     = 0;
unsigned long tUltRega   = 0;
unsigned long tWifiCheck = 0;
bool wifiEstavaOk        = true;
bool ntpIniciado         = false;
bool estadoRecuperado    = false;
bool primeiroEnvioFeito  = false;
bool otaConfigurado      = false;

WiFiUDP   ntpUDP;
NTPClient ntp(ntpUDP, "pool.ntp.org", -3 * 3600);
Preferences prefs;

// ── FUNÇÕES ────────────────────────────────────────────────

void valvulaAbrir()  { digitalWrite(VALVE_PIN, RELE_ATIVO_EM_LOW ? LOW  : HIGH); }
void valvulaFechar() { digitalWrite(VALVE_PIN, RELE_ATIVO_EM_LOW ? HIGH : LOW); }

void ledConectando()  { rgbLedWrite(RGB_BUILTIN, 255, 255, 255); }
void ledOcioso()      { rgbLedWrite(RGB_BUILTIN, 0,   0,   255); }
void ledEnchendo()    { rgbLedWrite(RGB_BUILTIN, 0,   255, 0);   }
void ledDrenando()    { rgbLedWrite(RGB_BUILTIN, 0,   255, 255); }
void ledErroWifi()    { rgbLedWrite(RGB_BUILTIN, 255, 0,   0);   }
void ledErroEnvio()   { rgbLedWrite(RGB_BUILTIN, 255, 255, 0);   }
void ledErroBuffer()  { rgbLedWrite(RGB_BUILTIN, 255, 0,   255); }
void ledOTA()         { rgbLedWrite(RGB_BUILTIN, 180, 0,   255); }

void atualizarLedEstado() {
  switch (estado) {
    case AGUARDANDO:
    case CONCLUIDO: ledOcioso();   break;
    case ENCHENDO:  ledEnchendo(); break;
    case DRENANDO:  ledDrenando(); break;
  }
}

void resetPlateau() {
  totalLeit = 0;
  janIdx    = 0;
  memset(janUmid, 0, sizeof(janUmid));
}

int mediana(const int* v) {
  int a[MEDIANA_N];
  memcpy(a, v, sizeof(a));
  for (int i = 0; i < MEDIANA_N; i++)
    for (int j = i + 1; j < MEDIANA_N; j++)
      if (a[j] < a[i]) { int t = a[i]; a[i] = a[j]; a[j] = t; }
  return a[MEDIANA_N / 2];
}

int horaDoDia() {
  if (!ntp.isTimeSet()) return -1;
  time_t t = ntp.getEpochTime();
  struct tm* ti = localtime(&t);
  return ti->tm_hour;
}

void salvarEstado() {
  if (!ntp.isTimeSet()) return;
  uint32_t agora = (uint32_t)ntp.getEpochTime();
  prefs.putUChar("estado", (uint8_t)estado);
  prefs.putInt("ciclos", numCiclos);
  prefs.putULong("startEpoch", agora - (millis() - tTeste)  / 1000UL);
  prefs.putULong("regaEpoch",  agora - (millis() - tUltRega) / 1000UL);
  if (estado == ENCHENDO) {
    prefs.putULong("cicloEpoch", agora - (millis() - tCiclo) / 1000UL);
  }
}

void limparEstadoSalvo() {
  prefs.remove("estado");
  prefs.remove("ciclos");
  prefs.remove("startEpoch");
  prefs.remove("regaEpoch");
  prefs.remove("cicloEpoch");
}

void recuperarEstadoSalvo() {
  if (estadoRecuperado) return;
  if (!ntp.isTimeSet()) return;
  estadoRecuperado = true;

  uint8_t salvo = prefs.getUChar("estado", (uint8_t)AGUARDANDO);
  if (salvo != ENCHENDO && salvo != DRENANDO) return;

  numCiclos = prefs.getInt("ciclos", 0);

  uint32_t agora     = (uint32_t)ntp.getEpochTime();
  uint32_t startEp   = prefs.getULong("startEpoch", agora);
  uint32_t regaEp    = prefs.getULong("regaEpoch",  agora);
  unsigned long decInicio = (agora > startEp) ? (agora - startEp) * 1000UL : 0;
  unsigned long decRega   = (agora > regaEp)  ? (agora - regaEp)  * 1000UL : 0;
  tTeste   = millis() - decInicio;
  tUltRega = millis() - decRega;
  tEnvio   = millis();

  if (salvo == ENCHENDO) {
    uint32_t cicloEp = prefs.getULong("cicloEpoch", agora);
    unsigned long decCiclo = (agora > cicloEp) ? (agora - cicloEp) * 1000UL : 0;
    if (decCiclo >= ENCHENDO_MAX_MS) {
      estado = DRENANDO; numCiclos++; valvulaFechar();
      Serial.println("[RECUPERACAO] Reinicio - teto do ciclo vencido - DRENANDO.");
    } else {
      tCiclo = millis() - decCiclo; estado = ENCHENDO; valvulaAbrir();
      Serial.println("[RECUPERACAO] Reinicio - retomando ENCHENDO.");
    }
  } else {
    estado = DRENANDO; valvulaFechar();
    Serial.println("[RECUPERACAO] Reinicio - retomando DRENANDO.");
  }
  resetPlateau();
  secoConsec = 0;
  salvarEstado();
}

void sincronizarNtp() {
  ntp.begin();
  for (int i = 0; i < 5 && !ntp.isTimeSet(); i++) ntp.update();
  ntpIniciado = true;
}

// Segredos: no firmware de cabo, grava os de segredos.h na NVS; nos dois
// casos, usa o que está na NVS.
void carregarSegredos() {
#ifndef BUILD_PUBLICO
  if (prefs.getString("wSsid",  "") != WIFI_SSID)  prefs.putString("wSsid",  WIFI_SSID);
  if (prefs.getString("wSenha", "") != WIFI_SENHA) prefs.putString("wSenha", WIFI_SENHA);
  if (prefs.getString("sUrl",   "") != SCRIPT_URL) prefs.putString("sUrl",   SCRIPT_URL);
  if (prefs.getString("oSenha", "") != OTA_SENHA)  prefs.putString("oSenha", OTA_SENHA);
#endif
  wifiSsid  = prefs.getString("wSsid",  "");
  wifiSenha = prefs.getString("wSenha", "");
  scriptUrl = prefs.getString("sUrl",   "");
  otaSenha  = prefs.getString("oSenha", "");
}

// ── Atualização pela internet: prova, confirmação e volta ──
void confirmarFirmware() {
  emProva = false;
  prefs.putBool("otaProva", false);
  prefs.putInt("otaBoots", 0);
  Serial.printf("[OTA-NET] Versao %d confirmada.\n", FW_VERSAO);
}

void reverterFirmware(const char* motivo) {
  valvulaFechar();
  Serial.printf("[OTA-NET] Versao %d com problema (%s) - voltando para a anterior.\n", FW_VERSAO, motivo);
  salvarEstado();
  prefs.putBool("otaProva", false);
  prefs.putInt("otaBoots", 0);
  prefs.putInt("fwRuim", FW_VERSAO);   // não baixa esta versão de novo
  const esp_partition_t* outra = esp_ota_get_next_update_partition(NULL);
  esp_app_desc_t desc;
  if (outra && esp_ota_get_partition_description(outra, &desc) == ESP_OK &&
      esp_ota_set_boot_partition(outra) == ESP_OK) {
    delay(300);
    ESP.restart();
  }
  Serial.println("[OTA-NET] Nao ha versao anterior valida - seguindo nesta.");
  emProva = false;
}

// Chamada logo no boot: conta quantas vezes a versão em prova já ligou.
void verificarProva() {
  if (!prefs.getBool("otaProva", false)) return;
  int boots = prefs.getInt("otaBoots", 0) + 1;
  prefs.putInt("otaBoots", boots);
  emProva = true;
  Serial.printf("[OTA-NET] Versao %d em prova (boot %d de %d).\n", FW_VERSAO, boots, OTA_PROVA_MAX_BOOTS);
  if (boots > OTA_PROVA_MAX_BOOTS) reverterFirmware("reiniciou varias vezes");
}

// Parâmetros de rega salvos (último valor recebido da config remota).
void carregarParamsRega() {
  regaLimiar = prefs.getInt("rLimiar", REGA_LIMIAR_PADRAO);
  regaMaxH   = prefs.getInt("rMaxH",   REGA_MAX_H_PADRAO);
  regaMinH   = prefs.getInt("rMinH",   REGA_MIN_H_PADRAO);
}

// Aplica limiar/maxH/minH vindos da config, só se estiverem nas faixas aceitas
// e com mínimo < máximo. Campo ausente (script antigo) = mantém o atual.
void aplicarParamsRega(JsonDocument& doc) {
  int lim = doc["limiar"] | regaLimiar;
  int mx  = doc["maxH"]   | regaMaxH;
  int mn  = doc["minH"]   | regaMinH;
  if (lim < LIMIAR_MIN || lim > LIMIAR_MAX) {
    Serial.printf("[CONFIG] limiar %d fora de %d-%d - ignorado\n", lim, LIMIAR_MIN, LIMIAR_MAX);
    lim = regaLimiar;
  }
  if (mx < MAX_H_MIN || mx > MAX_H_MAX) {
    Serial.printf("[CONFIG] maxH %d fora de %d-%d - ignorado\n", mx, MAX_H_MIN, MAX_H_MAX);
    mx = regaMaxH;
  }
  if (mn < MIN_H_MIN || mn > MIN_H_MAX) {
    Serial.printf("[CONFIG] minH %d fora de %d-%d - ignorado\n", mn, MIN_H_MIN, MIN_H_MAX);
    mn = regaMinH;
  }
  if (mn >= mx) {
    Serial.printf("[CONFIG] minH %d >= maxH %d - tempos ignorados\n", mn, mx);
    mn = regaMinH; mx = regaMaxH;
  }
  if (lim != regaLimiar || mx != regaMaxH || mn != regaMinH) {
    Serial.printf("[CONFIG] Rega: limiar %d->%d | max %dh->%dh | min %dh->%dh\n",
                  regaLimiar, lim, regaMaxH, mx, regaMinH, mn);
    regaLimiar = lim; regaMaxH = mx; regaMinH = mn;
    prefs.putInt("rLimiar", regaLimiar);
    prefs.putInt("rMaxH",   regaMaxH);
    prefs.putInt("rMinH",   regaMinH);
  }
}

// Consulta a config remota (GET no Apps Script). Atualiza testeAtivo,
// modoOperacao e os parâmetros de rega. Se falhar, MANTÉM a config atual
// (fail-safe). É assim que o dono, de outra cidade, troca Teste 3 <-> Teste 4,
// observa <-> cultivo e ajusta limiar/tempos da rega.
void consultarConfig() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  // informa a versão em uso: o dono vê no navegador qual firmware está rodando
  http.begin(client, scriptUrl + "?fwAtual=" + String(FW_VERSAO));
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // GET precisa ler o corpo
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      int novoTeste = doc["teste"] | testeAtivo;
      const char* m = doc["modo"] | "";
      int novoModo = modoOperacao;
      if (strcmp(m, "cultivo") == 0) novoModo = MODO_CULTIVO;
      else if (strcmp(m, "observa") == 0) novoModo = MODO_OBSERVA;

      if (novoTeste != testeAtivo || novoModo != modoOperacao) {
        Serial.printf("[CONFIG] Mudou: teste %d->%d, modo %s\n",
                      testeAtivo, novoTeste, novoModo == MODO_CULTIVO ? "CULTIVO" : "OBSERVA");
      }
      testeAtivo   = novoTeste;
      modoOperacao = novoModo;
      aplicarParamsRega(doc);
      fwPedido = doc["fw"] | 0;
      // falou com o Apps Script depois de 10 min ligada: versão nova aprovada
      if (emProva && millis() >= OTA_PROVA_CONFIRMA_MS) confirmarFirmware();
    }
  } else {
    Serial.printf("[CONFIG] GET falhou (HTTP %d) - mantendo config atual\n", code);
  }
  http.end();
}

void configurarOTA() {
  if (otaConfigurado) return;
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(otaSenha.c_str());
  ArduinoOTA.onStart([]() { valvulaFechar(); ledOTA();
    Serial.println("\n[OTA] Iniciada - valvula FECHADA."); });
  ArduinoOTA.onEnd([]()   { Serial.println("\n[OTA] Concluida. Reiniciando..."); });
  ArduinoOTA.onError([](ota_error_t e){ Serial.printf("[OTA] ERRO %u\n", e); atualizarLedEstado(); });
  ArduinoOTA.begin();
  otaConfigurado = true;
}

void checarWifi() {
  if (millis() - tWifiCheck < WIFI_CHECK_MS) return;
  tWifiCheck = millis();
  bool ok = (WiFi.status() == WL_CONNECTED);
  if (!ok) { ledErroWifi(); WiFi.reconnect(); }
  else if (!wifiEstavaOk) {
    if (!ntpIniciado) sincronizarNtp();
    configurarOTA();
    consultarConfig();
    recuperarEstadoSalvo();
    atualizarLedEstado();
  }
  wifiEstavaOk = ok;
}

bool botaoPressionado() {
  if (digitalRead(BTN_PIN) != LOW) return false;
  delay(50);
  if (digitalRead(BTN_PIN) != LOW) return false;
  while (digitalRead(BTN_PIN) == LOW) delay(10);
  return true;
}

int lerSensor(int pwrPin, int signalPin) {
  digitalWrite(pwrPin, HIGH);
  delay(150);
  int v = analogRead(signalPin);
  digitalWrite(pwrPin, LOW);
  delay(50);
  return v;
}

bool sensorValido(int v) { return v >= SENSOR_MIN_VALIDO && v <= SENSOR_MAX_VALIDO; }

// nValidos conta quantos sensores estão OK (0,1,2) — só pra registro/LED.
// A umidade de CONTROLE (que rege platô e gatilho de rega) é a do
// SENSOR_CONTROLE (o do ponto da planta), NÃO a média — os sensores estão
// em posições diferentes. Se o sensor de controle falhar, retorna -1 e o
// modo cultivo rega pelo tempo, no máximo a cada 24 h (garante água).
float umidadeControle(int s1f, int s2f, int* nValidos) {
  bool v1 = USAR_S1 && sensorValido(s1f);
  bool v2 = USAR_S2 && sensorValido(s2f);
  *nValidos = (v1 ? 1 : 0) + (v2 ? 1 : 0);
  int ctrl  = (SENSOR_CONTROLE == 1) ? s1f : s2f;
  bool ctrlOk = (SENSOR_CONTROLE == 1) ? v1 : v2;
  return ctrlOk ? (float)ctrl : -1.0f;
}

bool detectarPlato() {
  if (totalLeit < PLATEAU_WINDOW) return false;
  float mn = janUmid[0], mx = janUmid[0];
  for (int i = 1; i < PLATEAU_WINDOW; i++) {
    if (janUmid[i] < mn) mn = janUmid[i];
    if (janUmid[i] > mx) mx = janUmid[i];
  }
  return (mx - mn) <= PLATEAU_THRESHOLD;
}

void getTimestamp(char* out, size_t len) {
  ntp.update();
  time_t t = ntp.getEpochTime();
  struct tm* tm_info = localtime(&t);
  strftime(out, len, "%d/%m %H:%M:%S", tm_info);
}

int getDia() { return (int)((millis() - tTeste) / 86400000UL) + 1; }

bool enviarLote() {
  bool enviouAlgo = false;
  while (bufCount > 0 && WiFi.status() == WL_CONNECTED) {
    int n = min(bufCount, ENVIO_LOTE_MAX);
    DynamicJsonDocument doc(256 + (size_t)n * 160);
    doc["testDay"] = getDia();
    doc["teste"]   = testeAtivo;   // o script grava na planilha deste teste
    JsonArray arr  = doc.createNestedArray("readings");
    for (int i = 0; i < n; i++) {
      JsonObject r = arr.createNestedObject();
      r["timestamp"] = buf[i].timestamp;
      r["s1"]        = buf[i].s1;
      r["s2"]        = buf[i].s2;
      r["avg"]       = (float)((int)(buf[i].avg * 10 + 0.5f)) / 10.0f;
      r["valve"]     = buf[i].valveAberta ? "ABERTA" : "FECHADA";
      r["cycle"]     = buf[i].ciclo;
    }
    String json;
    serializeJson(doc, json);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, scriptUrl);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(json);
    http.end();

    bool sucesso = (code == 200 || code == 302);
    Serial.printf("[ENVIO] %d/%d -> HTTP %d (teste %d)\n", n, bufCount, code, testeAtivo);
    if (!sucesso) { ledErroEnvio(); return enviouAlgo; }

    int restante = bufCount - n;
    memmove(buf, buf + n, restante * sizeof(Leitura));
    bufCount = restante; enviouAlgo = true;
  }
  if (bufCount == 0) atualizarLedEstado();
  return enviouAlgo;
}

void encerrarCiclo(const char* motivo) {
  estado = DRENANDO; numCiclos++;
  valvulaFechar(); ledDrenando(); resetPlateau(); secoConsec = 0; salvarEstado();
  Serial.printf("[%s] Ciclo %d completo - Valvula FECHADA.\n", motivo, numCiclos);
}

void iniciarCiclo(const char* motivo) {
  estado = ENCHENDO; tCiclo = millis(); tUltRega = millis();
  valvulaAbrir(); ledEnchendo(); resetPlateau(); secoConsec = 0; salvarEstado();
  Serial.printf("[%s] Ciclo %d - Valvula ABERTA\n", motivo, numCiclos + 1);
}

// Baixa e grava a versão pedida pela config, se for mais nova. Nunca durante
// uma rega. O que estiver no buffer é enviado antes (some no reinício).
void verificarAtualizacao() {
  if (fwPedido <= FW_VERSAO || emProva) return;
  if (estado == ENCHENDO) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (fwPedido == prefs.getInt("fwRuim", 0)) return;
  if (fwPedido != fwTentada) { fwTentada = fwPedido; fwTentativas = 0; }
  if (fwTentativas >= OTA_MAX_TENTATIVAS) return;
  static unsigned long tUltTentativa = 0;
  if (fwTentativas > 0 && millis() - tUltTentativa < CONFIG_INTERVAL_MS) return;
  tUltTentativa = millis();
  fwTentativas++;

  String url = String(FW_URL_BASE) + fwPedido + "/firmware.bin";
  Serial.printf("[OTA-NET] Baixando versao %d (tentativa %d): %s\n", fwPedido, fwTentativas, url.c_str());
  enviarLote();
  valvulaFechar();
  ledOTA();

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // GitHub redireciona o download
  t_httpUpdate_return r = httpUpdate.update(client, url);
  if (r == HTTP_UPDATE_OK) {
    salvarEstado();
    prefs.putBool("otaProva", true);
    prefs.putInt("otaBoots", 0);
    Serial.println("[OTA-NET] Gravada. Reiniciando na versao nova...");
    delay(500);
    ESP.restart();
  }
  Serial.printf("[OTA-NET] Falhou (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
  atualizarLedEstado();
}

// ── SETUP ──────────────────────────────────────────────────

void setup() {
  prefs.begin("sis_teste3", false);
  carregarSegredos();
  carregarParamsRega();
  ledConectando();

  pinMode(VALVE_PIN, OUTPUT);
  valvulaFechar();

  Serial.begin(115200);
  unsigned long tSerial = millis();
  while (!Serial && millis() - tSerial < 3000) delay(10);
  verificarProva();

  Serial.println("\n=== SIS Teste 3/4 - Sensor RASO (config REMOTA) ===");
  Serial.println("Dois sensores rasos (~5cm). Modo OBSERVA(T3) / CULTIVO(T4) via config remota.");

  pinMode(S1_PWR,  OUTPUT);
  pinMode(S2_PWR,  OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  digitalWrite(S1_PWR, LOW);
  digitalWrite(S2_PWR, LOW);

  // RESET DE TESTE: se o botão estiver SEGURADO durante o boot, apaga o
  // estado salvo (de um teste anterior nesta mesma placa) e começa do zero
  // em AGUARDANDO. Sem isso, a placa "retomaria" o teste antigo (LED ciano
  // logo ao ligar) e gravaria numa aba/dia errados.
  delay(80); // deixa o pull-up assentar
  if (digitalRead(BTN_PIN) == LOW) {
    limparEstadoSalvo();
    estado           = AGUARDANDO;
    numCiclos        = 0;
    estadoRecuperado = true;   // impede a recuperação lá embaixo
    ledOcioso();
    Serial.println("[RESET] Botao segurado no boot - estado ZERADO, comecando do zero.");
    while (digitalRead(BTN_PIN) == LOW) delay(10); // espera soltar
  }

  Serial.printf("Firmware versao %d%s\n", FW_VERSAO, emProva ? " (em prova)" : "");
  if (wifiSsid.length() == 0) Serial.println("[ERRO] Sem WiFi salvo na placa: grave uma vez o firmware de cabo (com segredos.h).");
  WiFi.begin(wifiSsid.c_str(), wifiSenha.c_str());
  Serial.print("Conectando WiFi");
  unsigned long tWifi = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - tWifi < WIFI_TIMEOUT_MS) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(" OK | IP: %s\n", WiFi.localIP().toString().c_str());
    sincronizarNtp();
    configurarOTA();
    consultarConfig();
    wifiEstavaOk = true;
    recuperarEstadoSalvo();
    atualizarLedEstado();
  } else {
    Serial.println(" FALHOU - segue sem WiFi, reconectando em segundo plano.");
    wifiEstavaOk = false;
    ledErroWifi();
  }
  Serial.printf("Config atual: teste %d | modo %s | rega: limiar %d, max %dh, min %dh\n",
                testeAtivo, modoOperacao == MODO_CULTIVO ? "CULTIVO" : "OBSERVA",
                regaLimiar, regaMaxH, regaMinH);
  Serial.println("Pressione o botao (GPIO19 -> GND) para iniciar.");
}

void lerEfiltrar(int& s1, int& s2, int& s1f, int& s2f) {
  s1 = lerSensor(S1_PWR, SIGNAL1);
  s2 = lerSensor(S2_PWR, SIGNAL2);
  medS1[medIdx] = s1;
  medS2[medIdx] = s2;
  medIdx = (medIdx + 1) % MEDIANA_N;
  if (medIdx == 0) medCheia = true;
  s1f = medCheia ? mediana(medS1) : s1;
  s2f = medCheia ? mediana(medS2) : s2;
}

// ── LOOP ───────────────────────────────────────────────────

void loop() {
  ArduinoOTA.handle();
  checarWifi();

  // consulta config remota periodicamente
  if (millis() - tConfig >= CONFIG_INTERVAL_MS) {
    tConfig = millis();
    consultarConfig();
  }

  // versão nova que não se confirmou a tempo: volta para a anterior
  if (emProva && millis() > OTA_PROVA_LIMITE_MS) reverterFirmware("nao falou com o Apps Script em 20 min");
  verificarAtualizacao();

  // diagnóstico rápido no terminal
  if (millis() - tDiag >= DIAG_INTERVAL_MS) {
    tDiag = millis();
    int d1 = lerSensor(S1_PWR, SIGNAL1);
    int d2 = lerSensor(S2_PWR, SIGNAL2);
    Serial.printf("[DIAG] S1(GPIO0)=%d %-8s | S2(GPIO1)=%d %-8s\n",
      d1, sensorValido(d1) ? "OK" : "DEFEITO?",
      d2, sensorValido(d2) ? "OK" : "DEFEITO?");
  }

  // ── Aguarda botão ──────────────────────────────────────
  if (estado == AGUARDANDO) {
    if (millis() - tLeitura >= READ_INTERVAL_MS) {
      tLeitura = millis();
      int s1, s2, s1f, s2f; lerEfiltrar(s1, s2, s1f, s2f);
      Serial.printf("[AGUARDANDO] S1:%d  S2:%d\n", s1, s2);
    }
    if (botaoPressionado()) {
      enviarLote();
      bufCount = 0; numCiclos = 0;
      tTeste = millis(); tEnvio = millis();
      primeiroEnvioFeito = false;
      iniciarCiclo("INICIO");   // satura uma vez ao começar (nos dois modos)
    }
    return;
  }
  if (estado == CONCLUIDO) return;

  // Botão manual: ENCHENDO fecha; DRENANDO força rega.
  if (botaoPressionado()) {
    if (estado == ENCHENDO) encerrarCiclo("MANUAL");
    else                    iniciarCiclo("MANUAL");
  }

  // ── Leitura a cada 10 s ────────────────────────────────
  if (millis() - tLeitura >= READ_INTERVAL_MS) {
    tLeitura = millis();
    int s1, s2, s1f, s2f; lerEfiltrar(s1, s2, s1f, s2f);
    bool vAb = (estado == ENCHENDO);

    int   nVal;
    float umid = umidadeControle(s1f, s2f, &nVal);
    float avgGrava = (nVal > 0) ? umid : (s1 + s2) / 2.0f;

    if (nVal > 0) {
      janUmid[janIdx] = umid;
      janIdx = (janIdx + 1) % PLATEAU_WINDOW;
      totalLeit++;
    }

    char ts[20] = "";
    bool horaOk = ntp.isTimeSet();
    if (horaOk) {
      getTimestamp(ts, sizeof(ts));
      if (bufCount < BUF_MAX) {
        strncpy(buf[bufCount].timestamp, ts, sizeof(buf[bufCount].timestamp));
        buf[bufCount].s1 = s1; buf[bufCount].s2 = s2;
        buf[bufCount].avg = avgGrava; buf[bufCount].valveAberta = vAb;
        buf[bufCount].ciclo = numCiclos + 1;
        bufCount++;
      } else ledErroBuffer();
    }

    Serial.printf("[%s] S1:%d%s S2:%d%s Umid:%.1f(%dval) %s %s C:%d [%s]\n",
      horaOk ? ts : "sem-hora",
      s1, sensorValido(s1f) ? "" : "!", s2, sensorValido(s2f) ? "" : "!",
      (nVal > 0 ? umid : avgGrava), nVal,
      vAb ? "ABERTA" : "FECHADA",
      estado == ENCHENDO ? "ENCHENDO" : "DRENANDO", numCiclos + 1,
      modoOperacao == MODO_CULTIVO ? "CULTIVO" : "OBSERVA");

    if (nVal == 0) ledErroBuffer();

    // ── ENCHENDO: platô OU teto → fecha (nos dois modos) ──
    if (estado == ENCHENDO) {
      bool plato         = detectarPlato();
      bool tempoEsgotado = (millis() - tCiclo) >= ENCHENDO_MAX_MS;
      if (plato)              encerrarCiclo("SATURACAO");
      else if (tempoEsgotado) encerrarCiclo("SEGURANCA");
    }

    // ── DRENANDO ─────────────────────────────────────────
    else if (estado == DRENANDO) {
      // No modo OBSERVA (Teste 3, sem planta): NÃO rega mais. Só observa a
      // secagem (evaporação pura). Rega só se o dono apertar o botão.
      // No modo CULTIVO (Teste 4, com planta): rega por umidade.
      if (modoOperacao == MODO_CULTIVO) {
        // solo secou até o limiar de forma sustentada?
        bool sensorOk = (nVal > 0 && umid >= 0);
        if (sensorOk && umid >= regaLimiar) secoConsec++;
        else                                secoConsec = 0;

        // sensor da planta falhando: não confia no tempo longo, volta pra 24 h
        int maxH = sensorOk ? regaMaxH : min(regaMaxH, REGA_MAX_SEM_SENSOR_H);
        unsigned long desdeRega = millis() - tUltRega;
        bool minOk  = desdeRega >= (unsigned long)regaMinH * 3600000UL;   // anti-encharcamento
        bool secou  = (secoConsec >= REGA_CONSEC) && minOk;
        bool tempo  = desdeRega >= (unsigned long)maxH * 3600000UL;       // garante água

        if (secou)      iniciarCiclo("SOLO_SECO");
        else if (tempo) iniciarCiclo("AGENDADA_TEMPO");
      }
    }
  }

  // ── Envio em lote ────────────────────────────────────────
  unsigned long intervaloEnvio = primeiroEnvioFeito ? SEND_INTERVAL_MS : PRIMEIRO_ENVIO_MS;
  if (millis() - tEnvio >= intervaloEnvio) {
    tEnvio = millis();
    if (enviarLote()) primeiroEnvioFeito = true;
  }
}
