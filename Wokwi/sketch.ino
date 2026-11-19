// ApoiaSP - totem de apoio ao cidadao (ESP32 + Wokwi) - CitizenTech
// Grupo GS:
// Laura de Siqueira Silva - RM573796 
// Lavínia Tavares Apolinário Dantas - RM: 574172
// Vitória Valentim de Melo Ferreira - RM570502
// Yannick Dávila Parreira - RM572443
// Yasmin Siunte Gregório - RM569867
// Resumo: Esse programa simula um poste de ajuda, mostrando a direção mais próxima de pontos de emergência
//
// Arquitetura em 3 camadas:
//   1) Device IoT: este sketch (sensor ultrassonico, GPS, RTC, OLED, botoes, buzzer)
//   2) Back-end:   FIWARE Orion (NGSI-v2), acessado por um tunel Cloudflare
//   3) Dashboard:  index.html + server.py (painel do gestor) e totem.html (totem web)
//
// O totem funciona sozinho (offline). Quando ha rede, o estado e enviado ao Orion.

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>
#include <TinyGPSPlus.h>
#include "qrcode.h"

// Pinos

#define PIN_SDA 21
#define PIN_SCL 22
#define PIN_GPS_RX 16
#define PIN_GPS_TX 17
#define PIN_TRIG 5
#define PIN_ECHO 18
#define PIN_BUZZER 4
#define PIN_BTN_CIMA 25
#define PIN_BTN_BAIXO 27
#define PIN_BTN_OK 26
#define PIN_BTN_VOLTAR 14

// Configuracao

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_SENHA = "";

// ATENCAO: a URL do tunel Cloudflare muda toda vez que o cloudflared e reiniciado.
const char* FIWARE_ENDPOINT = "https://carb-else-treasurer-usc.trycloudflare.com";
const char* NOME_DISPOSITIVO = "poste-se-01";

const char* GOOGLE_MAPS_API_KEY = "SUA_CHAVE_GOOGLE_MAPS";
const char* GOOGLE_ROUTES_URL = "https://routes.googleapis.com/directions/v2:computeRoutes";

// Localizacao do poste: Metro Trianon-Masp, Av. Paulista
const double POSTE_LAT = -23.56336;
const double POSTE_LON = -46.65427;

// A tela liga quando alguem chega a 150 cm ou aperta algum botao
const float DIST_ATIVACAO_CM = 150.0;
const unsigned long TEMPO_DESLIGAR_MS = 30000UL;

#define MAX_ETAPAS 12
#define TAM_INSTRUCAO 90

// QR code do passo a passo: limite de texto por pagina e de paginas
#define QR_TEXTO_MAX 134
#define MAX_PAGINAS MAX_ETAPAS
#define QR_VERSAO_MIN 8
#define QR_VERSAO_MAX 10

// Bytes que cabem em cada versao do QR com ECC_LOW (v1..v10).
// A biblioteca NAO confere o tamanho: texto maior que a versao estoura a pilha
const uint16_t QR_CAPACIDADE[QR_VERSAO_MAX] = { 17, 32, 53, 78, 106, 134, 154, 192, 230, 271 };

// Tipos

enum Idioma { PT, ES, EN };

enum Tela {
  TELA_DESLIGADA,
  TELA_SPLASH,
  TELA_IDIOMA,
  TELA_CATEGORIA,
  TELA_TIPO,
  TELA_RESULTADO,
  TELA_ACOES,
  TELA_PASSOS,
  TELA_QR
};

enum Categoria { CAT_SAUDE, CAT_SEGURANCA, CAT_DOCUMENTOS, CAT_ASSISTENCIA };

enum TipoLocal {
  TIPO_UBS,
  TIPO_UPA,
  TIPO_PM,
  TIPO_DEFESA_CIVIL,
  TIPO_POUPATEMPO,
  TIPO_PF,
  TIPO_CRAS,
  TOTAL_TIPOS
};

enum Texto {
  T_CATEGORIA,
  T_SAUDE,
  T_SEGURANCA,
  T_DOCUMENTOS,
  T_ASSISTENCIA,
  T_QUAL_LOCAL,
  T_MAIS_PROXIMO,
  T_PASSOS,
  T_QR_MAPS,
  T_QR_PASSOS,
  T_ESCANEIE,
  TOTAL_TEXTOS
};

// Tipos de instrucao da rota offline (frase em FORMATOS)
enum Acao {
  A_SIGA,
  A_ESQ,
  A_DIR,
  A_ESQ_EM_DIRECAO,
  A_DIR_EM_DIRECAO,
  A_ESQ_PERMANECER,
  A_ESQ_SUAVE_PERMANECER,
  A_DIR_SUAVE,
  A_DIR_SUAVE_PERMANECER,
  A_CONTINUE,
  A_FAIXA,
  A_SUDESTE,
  A_ESQ_ESCADAS,
  A_ESQ_DEPOIS,
  TOTAL_ACOES
};

struct Tipo {
  const char* nome[3];
  Categoria categoria;
};

// Passo da rota offline; metros = distancia a andar neste passo
struct PassoFixo {
  Acao acao;
  const char* rua;
  uint16_t metros;
};

struct Local {
  const char* nome;
  double lat;
  double lon;
  TipoLocal tipo;
  const char* linkMaps;
  const PassoFixo* passos;
  uint8_t totalPassos;
};

struct EtapaRota {
  char instrucao[TAM_INSTRUCAO];
  uint16_t metros;
};

struct Rota {
  uint8_t total;
  uint16_t minutos;
  EtapaRota etapas[MAX_ETAPAS];
};

struct Botao {
  uint8_t pino;
  bool anterior;
  unsigned long tempo;
};

// Textos

const char* TEXTOS[3][TOTAL_TEXTOS] = {
  { "Do que precisa?", "Saude", "Seguranca", "Documentos", "Assist. Social",
    "Qual local?", "Mais proximo:", "Passos na tela", "QR Google Maps",
    "QR passo a passo", "Escaneie" },
  { "Que necesita?", "Salud", "Seguridad", "Documentos", "Asist. Social",
    "Que lugar?", "Mas cercano:", "Pasos en pantalla", "QR Google Maps",
    "QR paso a paso", "Escanea" },
  { "What do you need?", "Health", "Safety", "Documents", "Social Aid",
    "Which place?", "Nearest:", "Steps on screen", "QR Google Maps",
    "QR step by step", "Scan" }
};

// Frases das instrucoes offline; %s recebe o nome da rua
const char* FORMATOS[3][TOTAL_ACOES] = {
  { "Siga em direcao a %s",
    "Vire a esquerda na %s",
    "Vire a direita na %s",
    "Vire a esquerda em direcao a %s",
    "Vire a direita em direcao a %s",
    "Vire a esquerda para permanecer na %s",
    "Curva suave a esquerda para permanecer na %s",
    "Curva suave a direita na %s",
    "Curva suave a direita para permanecer na %s",
    "Continue para %s",
    "Utilize a faixa de pedestres",
    "Siga na direcao sudeste na %s",
    "Vire a esquerda na %s. Use as escadas",
    "Vire a esquerda depois de %s" },
  { "Sigue hacia %s",
    "Gira a la izquierda en %s",
    "Gira a la derecha en %s",
    "Gira a la izquierda hacia %s",
    "Gira a la derecha hacia %s",
    "Gira a la izquierda para seguir en %s",
    "Curva suave a la izquierda para seguir en %s",
    "Curva suave a la derecha en %s",
    "Curva suave a la derecha para seguir en %s",
    "Continua hacia %s",
    "Usa el paso de peatones",
    "Sigue al sureste por %s",
    "Gira a la izquierda en %s. Usa las escaleras",
    "Gira a la izquierda despues de %s" },
  { "Head toward %s",
    "Turn left onto %s",
    "Turn right onto %s",
    "Turn left toward %s",
    "Turn right toward %s",
    "Turn left to stay on %s",
    "Slight left to stay on %s",
    "Slight right onto %s",
    "Slight right to stay on %s",
    "Continue to %s",
    "Use the crosswalk",
    "Head southeast on %s",
    "Turn left onto %s. Use the stairs",
    "Turn left after %s" }
};

const char* NOMES_IDIOMA[3] = { "Portugues (Brasil)", "Espanol (Latam)", "English" };
// Versao sem parenteses para o Orion (ele recusa os caracteres ( ) < > " ' = ; em textos)
const char* NOMES_IDIOMA_FIWARE[3] = { "Portugues - Brasil", "Espanol - Latam", "English" };
const char* IDIOMA_CHAVE[3] = { "pt", "es", "en" };
const char* IDIOMA_API[3] = { "pt-BR", "es", "en" };
// Rotulo ao lado do QR: [modo][idioma][linha]; modo 0 = Maps, 1 = passo a passo
const char* ROTULO_QR[2][3][2] = {
  { { "Google", "Maps" }, { "Google", "Maps" }, { "Google", "Maps" } },
  { { "Passo a", "passo" }, { "Paso a", "paso" }, { "Step by", "step" } }
};

const char* CAT_CHAVE[4] = { "saude", "seguranca", "documentos", "assistencia" };

// Locais e rotas offline (a pe, a partir do poste)

const Tipo TIPOS[TOTAL_TIPOS] = {
  { { "UBS", "UBS", "UBS" }, CAT_SAUDE },
  { { "UPA", "UPA", "UPA" }, CAT_SAUDE },
  { { "Policia Militar", "Policia Militar", "Military Police" }, CAT_SEGURANCA },
  { { "Defesa Civil", "Defensa Civil", "Civil Defense" }, CAT_SEGURANCA },
  { { "Poupatempo", "Poupatempo", "Poupatempo" }, CAT_DOCUMENTOS },
  { { "Policia Federal", "Policia Federal", "Federal Police" }, CAT_DOCUMENTOS },
  { { "CRAS", "CRAS", "CRAS" }, CAT_ASSISTENCIA }
};

const PassoFixo ROTA_UBS[] = {
  { A_SIGA, "R. Pamplona", 220 },
  { A_ESQ, "Alameda Campinas", 270 },
  { A_DIR, "Alameda Ribeirao Preto", 190 },
  { A_ESQ, "R. Alm. Marques de Leao", 170 }
};

const PassoFixo ROTA_UPA[] = {
  { A_SIGA, "R. Pamplona", 1300 },
  { A_ESQ_EM_DIRECAO, "R. Treze de Maio", 70 },
  { A_ESQ, "R. Treze de Maio", 210 },
  { A_DIR, "Praca Amadeu Amaral", 12 },
  { A_ESQ_PERMANECER, "Praca Amadeu Amaral", 54 },
  { A_DIR, "R. Joao Juliao", 140 },
  { A_CONTINUE, "Viaduto Beneficencia Portuguesa", 240 },
  { A_ESQ, "Rua Vergueiro", 140 }
};

const PassoFixo ROTA_PM[] = {
  { A_SIGA, "R. Pamplona", 400 },
  { A_ESQ, "Alameda Joaquim Eugenio de Lima", 500 },
  { A_DIR_SUAVE, "R. dos Belgas", 93 },
  { A_ESQ, "Rua dos Ingleses", 220 },
  { A_DIR_SUAVE_PERMANECER, "Rua dos Ingleses", 36 }
};

const PassoFixo ROTA_DEFESA_CIVIL[] = {
  { A_SIGA, "R. Carlos Comenale", 130 },
  { A_ESQ_ESCADAS, "R. Sao Carlos do Pinhal", 290 },
  { A_DIR, "Rua Peixoto Gomide", 700 },
  { A_DIR, "R. Augusta", 400 },
  { A_ESQ_DEPOIS, "Lebanon Food", 270 },
  { A_ESQ, "R. da Consolacao", 53 }
};

const PassoFixo ROTA_POUPATEMPO[] = {
  { A_SIGA, "R. Pamplona", 71 },
  { A_FAIXA, "", 650 },
  { A_DIR_EM_DIRECAO, "R. Manuel da Nobrega", 15 },
  { A_ESQ_EM_DIRECAO, "R. Manuel da Nobrega", 5 },
  { A_DIR, "R. Manuel da Nobrega", 750 },
  { A_ESQ_SUAVE_PERMANECER, "R. Manuel da Nobrega", 750 },
  { A_ESQ_EM_DIRECAO, "Av. Srg. Mario Kozel Filho", 130 },
  { A_DIR_EM_DIRECAO, "Av. Srg. Mario Kozel Filho", 12 },
  { A_ESQ, "Av. Srg. Mario Kozel Filho", 38 },
  { A_DIR_EM_DIRECAO, "Av. Srg. Mario Kozel Filho", 13 },
  { A_ESQ, "Av. Srg. Mario Kozel Filho", 44 }
};

const PassoFixo ROTA_PF[] = {
  { A_SUDESTE, "Av. Paulista", 20 },
  { A_FAIXA, "", 59 }
};

const PassoFixo ROTA_CRAS[] = {
  { A_SIGA, "R. Pamplona", 350 },
  { A_FAIXA, "", 350 },
  { A_DIR_EM_DIRECAO, "R. Manuel da Nobrega", 15 },
  { A_ESQ_EM_DIRECAO, "R. Manuel da Nobrega", 5 },
  { A_DIR, "R. Manuel da Nobrega", 140 },
  { A_ESQ, "Alameda Santos", 500 },
  { A_CONTINUE, "R. Cubatao", 800 },
  { A_DIR, "Rua Eca de Queiroz", 450 },
  { A_ESQ, "R. Artur de Almeida", 29 }
};

#define PASSOS(rota) rota, sizeof(rota) / sizeof(PassoFixo)

// Nomes com ate 21 caracteres (largura do display)
const Local LOCAIS[] = {
  { "UBS N. Sra. do Brasil", -23.5626683, -46.6492757, TIPO_UBS,
    "https://maps.app.goo.gl/oebUmocQyVQLBtpU8", PASSOS(ROTA_UBS) },
  { "UPA Vergueiro", -23.5675198, -46.6395946, TIPO_UPA,
    "https://maps.app.goo.gl/Zis4bBkaLpncvnPXA", PASSOS(ROTA_UPA) },
  { "Base Pol. Comunitaria", -23.5604083, -46.6465821, TIPO_PM,
    "https://maps.app.goo.gl/kqqDFSonLWxtbV8RA", PASSOS(ROTA_PM) },
  { "Sec. Seguranca Urbana", -23.5502157, -46.6538779, TIPO_DEFESA_CIVIL,
    "https://maps.app.goo.gl/rVqyp6VjxtYJQEcy5", PASSOS(ROTA_DEFESA_CIVIL) },
  { "Poupatempo ALESP", -23.5797861, -46.6573789, TIPO_POUPATEMPO,
    "https://maps.app.goo.gl/eYsHstPML97cdMva9", PASSOS(ROTA_POUPATEMPO) },
  { "Juizado Esp. Federal", -23.5632783, -46.6549348, TIPO_PF,
    "https://maps.app.goo.gl/HwV32PWg7sKayUmU7", PASSOS(ROTA_PF) },
  { "CRAS Vila Mariana", -23.5803166, -46.6464865, TIPO_CRAS,
    "https://maps.app.goo.gl/1EJD7CHVpXn5xVUq6", PASSOS(ROTA_CRAS) }
};

const int TOTAL_LOCAIS = sizeof(LOCAIS) / sizeof(LOCAIS[0]);

// Objetos e estado

Adafruit_SSD1306 display(128, 64, &Wire, -1);
RTC_DS1307 rtc;
TinyGPSPlus gps;
Preferences memoria;

QRCode qr;
uint8_t qrBuffer[408];                      // modulos do QR (cabe a versao 10)
char qrTexto[272];                          // 271 bytes (limite da v10) + '\0'
bool qrOk = false;
int qrModo = 0;                             // 0 = link Maps, 1 = passo a passo
int qrPagina = 0;
int qrPaginas = 1;
uint8_t qrInicio[MAX_PAGINAS + 1];          // primeiro passo de cada pagina

Botao botaoCima = { PIN_BTN_CIMA, HIGH, 0 };
Botao botaoBaixo = { PIN_BTN_BAIXO, HIGH, 0 };
Botao botaoOk = { PIN_BTN_OK, HIGH, 0 };
Botao botaoVoltar = { PIN_BTN_VOLTAR, HIGH, 0 };

Tela tela = TELA_SPLASH;
Idioma idioma = PT;

int selecao = 0;
int totalItens = 0;
int categoriaSel = 0;
int tiposDaCategoria[4];
int tipoSel = 0;

int destino = 0;
double distanciaReta = 0;
Rota rota;
bool rotaOnline = false;
int passoAtual = 0;

DateTime agora;
bool gpsOk = false;
bool pessoaPerto = false;
int pessoasHoje = 0;
int diaContagem = 0;

// "online" e escrito pela tarefa da nuvem e lido pelo loop: por isso volatile
volatile bool online = false;

unsigned long entradaTela = 0;
unsigned long ultimaAtividade = 0;
unsigned long ultimoDesenho = 0;

// Utilidades

const char* t(Texto id) {
  return TEXTOS[idioma][id];
}

void formatarDistancia(char* saida, double metros) {
  if (metros < 1000) {
    snprintf(saida, 16, "%d m", (int)metros);
  } else {
    snprintf(saida, 16, "%.1f km", metros / 1000.0);
  }
}

void imprimirCentro(const char* texto, int y, int tamanho) {
  display.setTextSize(tamanho);
  display.setCursor((128 - (int)strlen(texto) * 6 * tamanho) / 2, y);
  display.print(texto);
  display.setTextSize(1);
}

// Quebra o texto em linhas de 21 caracteres (largura do display)
void imprimirTexto(const char* texto, int y, int maxLinhas) {
  char linha[22];

  for (int i = 0; i < maxLinhas && *texto; i++) {
    int fim = strlen(texto);

    if (fim > 21) {
      fim = 21;
      for (int k = 21; k > 0; k--) {
        if (texto[k] == ' ') {
          fim = k;
          break;
        }
      }
    }

    memcpy(linha, texto, fim);
    linha[fim] = '\0';

    display.setCursor(0, y + i * 8);
    display.print(linha);

    texto += fim;
    while (*texto == ' ') texto++;
  }
}

// O display so mostra ASCII: remove acentos (UTF-8) do texto do Google
void semAcentos(const char* origem, char* saida, size_t tamanho) {
  static const char TABELA[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
    "aaaaaaaceeeeiiiidnooooo/ouuuuyty";

  size_t n = 0;

  for (; *origem && n < tamanho - 1; origem++) {
    uint8_t c = *origem;

    if (c == 0xC3 && origem[1]) {
      c = *++origem;
      if (c >= 0x80 && c < 0xC0) saida[n++] = TABELA[c - 0x80];
    } else if (c < 0x80) {
      saida[n++] = (c < ' ') ? ' ' : c;
    }
  }

  saida[n] = '\0';
}

// O Orion recusa ( ) < > " ' = ; em valores de texto (erro HTTP 400).
// Aqui esses caracteres (e o %) viram %XX; o dashboard decodifica com decodeURIComponent.
void escaparParaOrion(const char* origem, char* saida, size_t tamanho) {
  size_t n = 0;

  for (; *origem && n + 4 < tamanho; origem++) {
    if (strchr("<>\"'=;()%", *origem)) {
      n += snprintf(saida + n, tamanho - n, "%%%02X", (unsigned)(uint8_t)*origem);
    } else {
      saida[n++] = *origem;
    }
  }

  saida[n] = '\0';
}

// Distancia em linha reta (haversine) do poste ate o local
double distanciaAoPoste(const Local& local) {
  const double R = 6371000.0;

  double p1 = radians(POSTE_LAT);
  double p2 = radians(local.lat);
  double dp = radians(local.lat - POSTE_LAT);
  double dl = radians(local.lon - POSTE_LON);

  double a = sin(dp / 2) * sin(dp / 2) +
             cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);

  return 2 * R * atan2(sqrt(a), sqrt(1 - a));
}

int maisProximo(int tipo) {
  int melhor = 0;
  double menor = 1e12;

  for (int i = 0; i < TOTAL_LOCAIS; i++) {
    if (LOCAIS[i].tipo != tipo) continue;

    double d = distanciaAoPoste(LOCAIS[i]);

    if (d < menor) {
      menor = d;
      melhor = i;
    }
  }

  return melhor;
}

// Relogio, GPS e sensor de presenca

void salvarContagem() {
  memoria.putInt("dia", diaContagem);
  memoria.putInt("pessoas", pessoasHoje);
}

void atualizarRelogio() {
  static unsigned long ultima = 0;

  if (millis() - ultima < 500) return;
  ultima = millis();

  agora = rtc.now();

  // Zera a contagem na virada do dia
  if (agora.day() != diaContagem) {
    diaContagem = agora.day();
    pessoasHoje = 0;
    salvarContagem();
  }
}

// Ajusta o RTC pela internet (UTC-3), uma vez por boot
void sincronizarRelogioNtp() {
  static bool iniciado = false;
  static bool sincronizado = false;
  static unsigned long ultima = 0;

  if (sincronizado || millis() - ultima < 3000) return;
  ultima = millis();

  if (!iniciado) {
    configTime(-3 * 3600, 0, "pool.ntp.org");
    iniciado = true;
  }

  struct tm hora;

  if (getLocalTime(&hora, 10)) {
    rtc.adjust(DateTime(hora.tm_year + 1900, hora.tm_mon + 1, hora.tm_mday,
                        hora.tm_hour, hora.tm_min, hora.tm_sec));
    sincronizado = true;
  }
}

float lerDistanciaCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  long duracao = pulseIn(PIN_ECHO, HIGH, 25000);

  if (duracao == 0) return 999;

  return duracao * 0.0343 / 2;
}

// Enquanto houver alguem perto, a tela nao desliga
void atualizarPresenca() {
  static unsigned long ultima = 0;

  if (millis() - ultima < 200) return;
  ultima = millis();

  pessoaPerto = lerDistanciaCm() < DIST_ATIVACAO_CM;

  if (pessoaPerto) ultimaAtividade = millis();
}

// O poste e fixo: o GPS so indica se o modulo esta com sinal
void atualizarGps() {
  while (Serial2.available() > 0) {
    gps.encode(Serial2.read());
  }

  gpsOk = gps.location.isValid() && gps.location.age() < 5000;
}

// Rota offline (texto fixo)

void carregarRotaOffline() {
  const Local& local = LOCAIS[destino];
  int total = 0;

  rota.total = local.totalPassos;

  for (int i = 0; i < rota.total; i++) {
    const PassoFixo& passo = local.passos[i];

    snprintf(rota.etapas[i].instrucao, TAM_INSTRUCAO,
             FORMATOS[idioma][passo.acao], passo.rua);
    rota.etapas[i].metros = passo.metros;

    total += passo.metros;
  }

  rota.minutos = (total + 79) / 80;  // ~80 m/min a pe
}

// Google Routes API (rota online)

void desenharCarregando() {
  display.clearDisplay();
  imprimirCentro("...", 24, 3);
  display.display();
}

bool buscarRotaGoogle() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (strcmp(GOOGLE_MAPS_API_KEY, "SUA_CHAVE_GOOGLE_MAPS") == 0) return false;

  desenharCarregando();

  WiFiClientSecure clienteGoogle;
  clienteGoogle.setInsecure();  // em producao, validar o certificado

  HTTPClient http;

  if (!http.begin(clienteGoogle, GOOGLE_ROUTES_URL)) return false;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Goog-Api-Key", GOOGLE_MAPS_API_KEY);
  http.addHeader("X-Goog-FieldMask",
                 "routes.duration,"
                 "routes.legs.steps.distanceMeters,"
                 "routes.legs.steps.navigationInstruction.instructions");

  char corpo[400];

  snprintf(corpo, sizeof(corpo),
           "{\"origin\":{\"location\":{\"latLng\":{\"latitude\":%.7f,\"longitude\":%.7f}}},"
           "\"destination\":{\"location\":{\"latLng\":{\"latitude\":%.7f,\"longitude\":%.7f}}},"
           "\"travelMode\":\"WALK\",\"languageCode\":\"%s\",\"units\":\"METRIC\"}",
           POSTE_LAT, POSTE_LON,
           LOCAIS[destino].lat, LOCAIS[destino].lon,
           IDIOMA_API[idioma]);

  int codigo = http.POST((uint8_t*)corpo, strlen(corpo));

  if (codigo != HTTP_CODE_OK) {
    Serial.printf("Routes API: erro %d\n", codigo);
    http.end();
    return false;
  }

  String resposta = http.getString();
  http.end();

  JsonDocument doc;

  if (deserializeJson(doc, resposta)) return false;

  JsonArray passos = doc["routes"][0]["legs"][0]["steps"].as<JsonArray>();

  if (passos.isNull()) return false;

  int n = 0;

  for (JsonObject passo : passos) {
    if (n >= MAX_ETAPAS) break;

    const char* instrucao = passo["navigationInstruction"]["instructions"] | "";

    if (*instrucao == '\0') continue;

    semAcentos(instrucao, rota.etapas[n].instrucao, TAM_INSTRUCAO);
    rota.etapas[n].metros = passo["distanceMeters"] | 0;
    n++;
  }

  if (n == 0) return false;

  rota.total = n;
  rota.minutos = (int)ceil(atof(doc["routes"][0]["duration"] | "0s") / 60.0);

  return true;
}

// FIWARE Orion: estado atual do totem via NGSI-v2

const char* nomePaginaFiware() {
  switch (tela) {
    case TELA_DESLIGADA: return "desligada";
    case TELA_SPLASH: return "abertura";
    case TELA_IDIOMA: return "idioma";
    case TELA_CATEGORIA: return "categoria";
    case TELA_TIPO: return "servico";
    case TELA_RESULTADO: return "resultado";
    case TELA_ACOES: return "acoes";
    case TELA_PASSOS: return "rota";
    case TELA_QR: return "qrcode";
    default: return "desconhecida";
  }
}

const char* tituloPaginaFiware() {
  switch (tela) {
    case TELA_SPLASH: return "ApoiaSP";
    case TELA_IDIOMA: return "Idioma / Language";
    case TELA_CATEGORIA: return t(T_CATEGORIA);
    case TELA_TIPO: return t(T_QUAL_LOCAL);
    case TELA_RESULTADO: return t(T_MAIS_PROXIMO);
    case TELA_ACOES:
    case TELA_PASSOS:
    case TELA_QR: return LOCAIS[destino].nome;
    default: return "ApoiaSP";
  }
}

void atributoTexto(JsonDocument& entidade, const char* nome, const char* valor) {
  JsonObject atributo = entidade[nome].to<JsonObject>();
  atributo["type"] = "Text";
  atributo["value"] = valor;
}

void atributoNumero(JsonDocument& entidade, const char* nome, long valor) {
  JsonObject atributo = entidade[nome].to<JsonObject>();
  atributo["type"] = "Number";
  atributo["value"] = valor;
}

void atributoBooleano(JsonDocument& entidade, const char* nome, bool valor) {
  JsonObject atributo = entidade[nome].to<JsonObject>();
  atributo["type"] = "Boolean";
  atributo["value"] = valor;
}

void adicionarOpcoesFiware(JsonDocument& entidade) {
  JsonObject atributo = entidade["options"].to<JsonObject>();
  atributo["type"] = "StructuredValue";
  JsonArray opcoes = atributo["value"].to<JsonArray>();

  switch (tela) {
    case TELA_IDIOMA:
      for (int i = 0; i < 3; i++) opcoes.add(NOMES_IDIOMA_FIWARE[i]);
      break;
    case TELA_CATEGORIA:
      opcoes.add(t(T_SAUDE));
      opcoes.add(t(T_SEGURANCA));
      opcoes.add(t(T_DOCUMENTOS));
      opcoes.add(t(T_ASSISTENCIA));
      break;
    case TELA_TIPO:
      for (int i = 0; i < totalItens; i++) {
        opcoes.add(TIPOS[tiposDaCategoria[i]].nome[idioma]);
      }
      break;
    case TELA_ACOES:
      opcoes.add(t(T_PASSOS));
      opcoes.add(t(T_QR_MAPS));
      opcoes.add(t(T_QR_PASSOS));
      break;
    default:
      break;
  }
}

void montarEstadoFiware(JsonDocument& entidade) {
  // Buffers static: o ArduinoJson guarda o ponteiro ate o serializeJson
  static char qrEscapado[3 * sizeof(qrTexto) + 1];
  static char instrucaoEscapada[3 * TAM_INSTRUCAO + 1];

  entidade["id"] = "ApoiaSP:poste-se-01";
  entidade["type"] = "TotemApoiaSP";

  atributoTexto(entidade, "device", NOME_DISPOSITIVO);
  atributoTexto(entidade, "page", nomePaginaFiware());
  atributoTexto(entidade, "title", tituloPaginaFiware());
  atributoTexto(entidade, "language", IDIOMA_CHAVE[idioma]);
  atributoBooleano(entidade, "online", online);
  atributoNumero(entidade, "selection", selecao);
  adicionarOpcoesFiware(entidade);

  atributoTexto(entidade, "category", CAT_CHAVE[categoriaSel]);
  atributoTexto(entidade, "service_type", TIPOS[tipoSel].nome[idioma]);
  atributoTexto(entidade, "destination", LOCAIS[destino].nome);
  atributoNumero(entidade, "distance_m", (long)distanciaReta);
  atributoBooleano(entidade, "route_online", rotaOnline);
  atributoNumero(entidade, "route_minutes", rota.minutos);
  atributoNumero(entidade, "step_index", passoAtual + 1);
  atributoNumero(entidade, "step_count", rota.total);
  atributoTexto(entidade, "instruction", "");
  atributoNumero(entidade, "step_distance_m", 0);
  atributoNumero(entidade, "people_today", pessoasHoje);
  atributoBooleano(entidade, "gps_ok", gpsOk);
  atributoTexto(entidade, "qr_mode", qrModo == 0 ? "maps" : "steps");
  atributoNumero(entidade, "qr_page", qrPagina + 1);
  atributoNumero(entidade, "qr_pages", qrPaginas);

  // qr_text tem parenteses no modo passo a passo: vai codificado (%XX)
  escaparParaOrion(qrTexto, qrEscapado, sizeof(qrEscapado));
  atributoTexto(entidade, "qr_text", (tela == TELA_QR && qrOk) ? qrEscapado : "");

  // Batimento: muda a cada 10 s e prova ao dashboard que o totem continua enviando
  atributoNumero(entidade, "heartbeat", (long)(millis() / 10000UL));

  char horario[16];
  snprintf(horario, sizeof(horario), "%02d:%02d %02d/%02d",
           agora.hour(), agora.minute(), agora.day(), agora.month());
  atributoTexto(entidade, "display_time", horario);

  if (tela == TELA_PASSOS && rota.total > 0 && passoAtual < rota.total) {
    escaparParaOrion(rota.etapas[passoAtual].instrucao,
                     instrucaoEscapada, sizeof(instrucaoEscapada));

    JsonObject instrucao = entidade["instruction"].to<JsonObject>();
    instrucao["type"] = "Text";
    instrucao["value"] = instrucaoEscapada;
    JsonObject metros = entidade["step_distance_m"].to<JsonObject>();
    metros["type"] = "Number";
    metros["value"] = rota.etapas[passoAtual].metros;
  }
}

// Cliente TLS e HTTP globais: evita criar/destruir objetos pesados a cada envio
WiFiClientSecure clienteFiware;
HTTPClient httpFiware;

bool enviarEstadoFiware(const char* corpo, size_t tamanho) {
  static bool configurado = false;

  if (!configurado) {
    clienteFiware.setInsecure();  // prototipo: usar certificado validado em producao
    configurado = true;
  }

  String url = String(FIWARE_ENDPOINT) + "/v2/entities?options=upsert";

  if (!httpFiware.begin(clienteFiware, url)) return false;

  httpFiware.setConnectTimeout(4000);
  httpFiware.setTimeout(4000);
  httpFiware.addHeader("Content-Type", "application/json");
  httpFiware.addHeader("Accept", "application/json");

  int codigo = httpFiware.POST((uint8_t*)corpo, tamanho);
  Serial.printf("FIWARE HTTP: %d | heap livre: %u\n", codigo, ESP.getFreeHeap());

  if (codigo == 400) {
    // O Orion explica o motivo da recusa; mostra tambem o que foi enviado
    Serial.println(httpFiware.getString());
    Serial.println(corpo);
  }

  httpFiware.end();

  // Erro de rede: descarta a conexao para a proxima tentativa comecar limpa
  if (codigo < 0) clienteFiware.stop();

  return codigo == 201 || codigo == 204;
}

// ---- Nuvem em tarefa propria (nucleo 0): a rede pode demorar ou falhar
// sem travar botoes, display e sensores, que rodam no loop() (nucleo 1).

char corpoFila[2048];                 // JSON pronto para enviar
size_t tamanhoFila = 0;
volatile bool filaCheia = false;      // true = ha um estado esperando envio
volatile bool envioFalhou = false;

void tarefaNuvem(void*) {
  for (;;) {
    if (filaCheia) {
      bool ok = enviarEstadoFiware(corpoFila, tamanhoFila);

      online = ok;
      if (!ok) envioFalhou = true;

      filaCheia = false;              // so libera o buffer depois do envio
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// O loop so conecta o Wi-Fi, monta o estado e o enfileira; quem envia e a tarefa.
void atualizarNuvem() {
  static unsigned long ultimaWifi = 0;
  static unsigned long ultimaTentativa = 0;
  static unsigned long ultimoEnvio = 0;
  static char ultimoEstadoEnfileirado[2048] = "";

  if (WiFi.status() != WL_CONNECTED) {
    online = false;

    // Diagnostico: mostra no Serial por que nada esta sendo enviado
    static unsigned long ultimoAviso = 0;
    if (millis() - ultimoAviso > 3000) {
      ultimoAviso = millis();
      Serial.printf("Wi-Fi sem conexao (status %d): nada sera enviado ao Orion\n", WiFi.status());
    }

    if (millis() - ultimaWifi > 20000) {
      WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);
      ultimaWifi = millis();
    }
    return;
  }

  static bool avisouConexao = false;
  if (!avisouConexao) {
    avisouConexao = true;
    Serial.printf("Wi-Fi conectado, IP %s\n", WiFi.localIP().toString().c_str());
  }

  sincronizarRelogioNtp();

  if (filaCheia) return;              // a tarefa ainda esta enviando o anterior

  if (envioFalhou) {                  // falhou: forca reenviar o estado atual
    envioFalhou = false;
    ultimoEstadoEnfileirado[0] = '\0';
  }

  unsigned long intervalo = online ? 500UL : 5000UL;
  if (millis() - ultimaTentativa < intervalo) return;

  JsonDocument entidade;
  montarEstadoFiware(entidade);

  size_t tamanho = serializeJson(entidade, corpoFila, sizeof(corpoFila));

  if (tamanho == 0 || tamanho >= sizeof(corpoFila)) {
    Serial.println("Estado FIWARE maior que o limite");
    return;
  }

  bool mudou = strcmp(corpoFila, ultimoEstadoEnfileirado) != 0;
  bool passouHeartbeat = millis() - ultimoEnvio >= 15000UL;

  if (!mudou && !passouHeartbeat) return;

  strlcpy(ultimoEstadoEnfileirado, corpoFila, sizeof(ultimoEstadoEnfileirado));
  tamanhoFila = tamanho;
  ultimaTentativa = millis();
  ultimoEnvio = millis();

  __sync_synchronize();               // garante que o buffer esta pronto antes do aviso
  filaCheia = true;                   // a tarefa pega daqui
}

// QR (link do Maps ou passo a passo em texto)

#define LINHA_PASSO "%d. %s (%d m)\n"

// Divide os passos em paginas; cada pagina vira um QR com texto copia e cola
void paginarPassos() {
  int base = strlen(LOCAIS[destino].nome) + 10;  // cabecalho "Nome (1/3)\n"
  int usado = base;

  qrPaginas = 1;
  qrInicio[0] = 0;

  for (int i = 0; i < rota.total; i++) {
    int n = snprintf(NULL, 0, LINHA_PASSO, i + 1,
                     rota.etapas[i].instrucao, rota.etapas[i].metros);

    // i > inicio da pagina: nunca cria pagina vazia
    if (usado + n > QR_TEXTO_MAX && i > qrInicio[qrPaginas - 1] &&
        qrPaginas < MAX_PAGINAS) {
      qrInicio[qrPaginas++] = i;
      usado = base;
    }

    usado += n;
  }

  qrInicio[qrPaginas] = rota.total;
}

void montarTextoPassos(char* saida, size_t tamanho) {
  size_t n = snprintf(saida, tamanho, "%s (%d/%d)\n",
                      LOCAIS[destino].nome, qrPagina + 1, qrPaginas);

  for (int i = qrInicio[qrPagina]; i < qrInicio[qrPagina + 1]; i++) {
    if (n >= tamanho) break;

    n += snprintf(saida + n, tamanho - n, LINHA_PASSO, i + 1,
                  rota.etapas[i].instrucao, rota.etapas[i].metros);
  }
}

// Gera o QR da pagina atual (so quando muda, nao a cada desenho).
// Usa a menor versao em que o texto cabe: a biblioteca nao confere o
// tamanho e, se o texto for maior que a versao, corrompe a pilha.
void prepararQr() {
  if (qrModo == 0) {
    strlcpy(qrTexto, LOCAIS[destino].linkMaps, sizeof(qrTexto));
  } else {
    montarTextoPassos(qrTexto, sizeof(qrTexto));
  }

  size_t tamanho = strlen(qrTexto);

  qrOk = false;

  for (uint8_t v = QR_VERSAO_MIN; v <= QR_VERSAO_MAX && !qrOk; v++) {
    if (tamanho <= QR_CAPACIDADE[v - 1]) {
      qrOk = qrcode_initText(&qr, qrBuffer, v, ECC_LOW, qrTexto) == 0;
    }
  }
}

// Navegacao entre telas

void irPara(Tela novaTela) {
  tela = novaTela;
  entradaTela = millis();
  selecao = 0;
  totalItens = 0;

  switch (tela) {
    case TELA_IDIOMA:
      totalItens = 3;
      break;

    case TELA_CATEGORIA:
      totalItens = 4;
      break;

    case TELA_ACOES:
      totalItens = 3;
      break;

    case TELA_TIPO:
      for (int i = 0; i < TOTAL_TIPOS; i++) {
        if (TIPOS[i].categoria == categoriaSel) {
          tiposDaCategoria[totalItens++] = i;
        }
      }
      break;

    case TELA_QR:
      qrPagina = 0;
      if (qrModo == 1) paginarPassos();
      prepararQr();
      break;

    default:
      break;
  }
}

// Escolhe o local mais proximo e busca a rota:
// online (Google) ou, sem internet/API, o texto fixo offline
void buscarDestino() {
  destino = maisProximo(tipoSel);
  distanciaReta = distanciaAoPoste(LOCAIS[destino]);
  passoAtual = 0;

  rotaOnline = buscarRotaGoogle();

  if (!rotaOnline) carregarRotaOffline();

  Serial.printf("Consulta: %s / %s\n", CAT_CHAVE[TIPOS[tipoSel].categoria], LOCAIS[destino].nome);
}

// Liga a tela e comeca uma nova sessao
void iniciarSessao() {
  display.ssd1306_command(SSD1306_DISPLAYON);

  pessoasHoje++;
  salvarContagem();
  Serial.printf("Totem ativado; pessoas hoje: %d\n", pessoasHoje);

  ultimaAtividade = millis();
  irPara(TELA_SPLASH);
}

void desligarTela() {
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  irPara(TELA_DESLIGADA);
}

// Botoes

bool apertou(Botao& b) {
  bool atual = digitalRead(b.pino);

  if (atual != b.anterior && millis() - b.tempo > 50) {
    b.tempo = millis();
    b.anterior = atual;
    return atual == LOW;
  }

  return false;
}

void tratarBotoes() {
  bool cima = apertou(botaoCima);
  bool baixo = apertou(botaoBaixo);
  bool ok = apertou(botaoOk);
  bool voltar = apertou(botaoVoltar);

  if (!(cima || baixo || ok || voltar)) return;

  ultimaAtividade = millis();
  tone(PIN_BUZZER, 1500, 40);

  if (tela == TELA_DESLIGADA) {
    iniciarSessao();
    return;
  }

  if (tela == TELA_SPLASH) return;

  int dir = (int)baixo - (int)cima;  // -1 sobe, +1 desce

  if (tela == TELA_PASSOS) {
    passoAtual = constrain(passoAtual + dir, 0, rota.total - 1);
  } else if (tela == TELA_QR) {
    int pagina = constrain(qrPagina + dir, 0, qrPaginas - 1);

    if (qrModo == 1 && pagina != qrPagina) {
      qrPagina = pagina;
      prepararQr();
    }
  } else if (totalItens > 0) {
    selecao = (selecao + totalItens + dir) % totalItens;
  }

  switch (tela) {
    case TELA_IDIOMA:
      if (ok) {
        idioma = (Idioma)selecao;
        irPara(TELA_CATEGORIA);
      }
      break;

    case TELA_CATEGORIA:
      if (ok) {
        categoriaSel = selecao;
        irPara(TELA_TIPO);
      } else if (voltar) {
        irPara(TELA_IDIOMA);
      }
      break;

    case TELA_TIPO:
      if (ok) {
        tipoSel = tiposDaCategoria[selecao];
        buscarDestino();
        irPara(TELA_RESULTADO);
      } else if (voltar) {
        irPara(TELA_CATEGORIA);
      }
      break;

    case TELA_RESULTADO:
      if (ok) {
        irPara(TELA_ACOES);
      } else if (voltar) {
        irPara(TELA_TIPO);
      }
      break;

    case TELA_ACOES:
      if (ok) {
        if (selecao == 0) {
          irPara(TELA_PASSOS);
        } else {
          qrModo = selecao - 1;  // 1 = Maps (modo 0), 2 = passo a passo (modo 1)
          irPara(TELA_QR);
        }
      } else if (voltar) {
        irPara(TELA_RESULTADO);
      }
      break;

    case TELA_PASSOS:
    case TELA_QR:
      if (voltar) irPara(TELA_ACOES);
      break;

    default:
      break;
  }
}

// Liga com pessoa a menos de 150 cm; desliga apos 30 s sem botao nem pessoa
void controlarTela() {
  if (tela == TELA_DESLIGADA) {
    if (pessoaPerto) iniciarSessao();
  } else if (tela == TELA_SPLASH) {
    if (millis() - entradaTela > 2500) irPara(TELA_IDIOMA);
  } else if (millis() - ultimaAtividade > TEMPO_DESLIGAR_MS) {
    desligarTela();
  }
}

// Desenho das telas

void desenharCabecalho() {
  char linha[16];

  snprintf(linha, sizeof(linha), "%02d:%02d %02d/%02d",
           agora.hour(), agora.minute(), agora.day(), agora.month());

  display.setCursor(0, 0);
  display.print(linha);

  const char* estado = online ? "ON" : "OFF";

  display.setCursor(128 - 6 * strlen(estado), 0);
  display.print(estado);

  display.drawLine(0, 10, 127, 10, WHITE);
}

void desenharMenu(const char* titulo, const char* const itens[], int total) {
  desenharCabecalho();

  display.setCursor(0, 13);
  display.print(titulo);

  for (int i = 0; i < total; i++) {
    int y = 25 + i * 10;

    if (i == selecao) {
      display.fillRect(0, y - 1, 128, 10, WHITE);
      display.setTextColor(BLACK);
    } else {
      display.setTextColor(WHITE);
    }

    display.setCursor(2, y);
    display.print(itens[i]);
  }

  display.setTextColor(WHITE);
}

void desenharResultado() {
  char dist[16];
  char linha[24];

  formatarDistancia(dist, distanciaReta);

  desenharCabecalho();

  display.setCursor(0, 13);
  display.print(t(T_MAIS_PROXIMO));

  display.setCursor(0, 23);
  display.print(LOCAIS[destino].nome);

  display.setTextSize(2);
  display.setCursor(0, 34);
  display.print(dist);
  display.setTextSize(1);

  snprintf(linha, sizeof(linha), "%s  %d min",
           rotaOnline ? "ONLINE" : "OFFLINE", rota.minutos);
  display.setCursor(0, 55);
  display.print(linha);
}

void desenharPassos() {
  char linha[24];
  char dist[16];

  desenharCabecalho();

  snprintf(linha, sizeof(linha), "%d/%d  %s", passoAtual + 1, rota.total,
           rotaOnline ? "ONLINE" : "OFFLINE");
  display.setCursor(0, 13);
  display.print(linha);

  imprimirTexto(rota.etapas[passoAtual].instrucao, 23, 4);

  formatarDistancia(dist, rota.etapas[passoAtual].metros);
  display.setCursor(0, 56);
  display.print(dist);
}

// Desenha o QR ja gerado por prepararQr()
void desenharQr() {
  if (!qrOk) {
    display.setCursor(0, 20);
    display.print("QR erro");
    return;
  }

  int escala = max(1, 64 / (qr.size + 2));
  int margem = (64 - qr.size * escala) / 2;

  display.fillRect(0, 0, 64, 64, WHITE);

  for (uint8_t y = 0; y < qr.size; y++) {
    for (uint8_t x = 0; x < qr.size; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        display.fillRect(margem + x * escala, margem + y * escala,
                         escala, escala, BLACK);
      }
    }
  }

  display.setCursor(68, 4);
  display.print(t(T_ESCANEIE));

  display.setCursor(68, 16);
  display.print(ROTULO_QR[qrModo][idioma][0]);

  display.setCursor(68, 26);
  display.print(ROTULO_QR[qrModo][idioma][1]);

  if (qrModo == 1 && qrPaginas > 1) {
    char pagina[12];

    snprintf(pagina, sizeof(pagina), "%d/%d ^v", qrPagina + 1, qrPaginas);
    display.setCursor(68, 48);
    display.print(pagina);
  }
}

void desenharTela() {
  if (tela == TELA_DESLIGADA) return;

  if (millis() - ultimoDesenho < 300) return;
  ultimoDesenho = millis();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  switch (tela) {
    case TELA_SPLASH: {
      imprimirCentro("ApoiaSP", 16, 2);
      imprimirCentro("CitizenTech", 44, 1);
      break;
    }
    case TELA_IDIOMA:
      desenharMenu("Idioma / Language", NOMES_IDIOMA, 3);
      break;
    case TELA_CATEGORIA: {
      const char* itens[4] = { t(T_SAUDE), t(T_SEGURANCA), t(T_DOCUMENTOS), t(T_ASSISTENCIA) };
      desenharMenu(t(T_CATEGORIA), itens, 4);
      break;
    }
    case TELA_TIPO: {
      const char* itens[4];
      for (int i = 0; i < totalItens; i++) {
        itens[i] = TIPOS[tiposDaCategoria[i]].nome[idioma];
      }
      desenharMenu(t(T_QUAL_LOCAL), itens, totalItens);
      break;
    }
    case TELA_RESULTADO:
      desenharResultado();
      break;
    case TELA_ACOES: {
      const char* itens[3] = { t(T_PASSOS), t(T_QR_MAPS), t(T_QR_PASSOS) };
      desenharMenu(LOCAIS[destino].nome, itens, 3);
      break;
    }
    case TELA_PASSOS:
      desenharPassos();
      break;
    case TELA_QR:
      desenharQr();
      break;
    default:
      break;
  }

  display.display();
}

// Programa principal

void setup() {
  Serial.begin(115200);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BTN_CIMA, INPUT_PULLUP);
  pinMode(PIN_BTN_BAIXO, INPUT_PULLUP);
  pinMode(PIN_BTN_OK, INPUT_PULLUP);
  pinMode(PIN_BTN_VOLTAR, INPUT_PULLUP);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Falha no OLED");
    while (true) delay(1000);
  }

  if (!rtc.begin()) Serial.println("RTC nao encontrado");

  if (!rtc.isrunning()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  Serial2.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  memoria.begin("apoiasp", false);
  diaContagem = memoria.getInt("dia", 0);
  pessoasHoje = memoria.getInt("pessoas", 0);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);

  // Tarefa que envia o estado ao Orion (pilha grande: o TLS precisa)
  xTaskCreatePinnedToCore(tarefaNuvem, "nuvem", 16384, NULL, 1, NULL, 0);

  agora = rtc.now();
  ultimaAtividade = millis();
  irPara(TELA_SPLASH);

  Serial.println("ApoiaSP iniciado");
}

void loop() {
  atualizarGps();
  atualizarRelogio();
  atualizarPresenca();
  atualizarNuvem();
  tratarBotoes();
  controlarTela();
  desenharTela();
}
