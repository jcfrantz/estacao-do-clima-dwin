// Estação do Clima — ESP32 + DHT11 + display DWIN (DGUS)
// Mostra o desenho das crianças conforme a temperatura e a umidade.
// Monitor Serial (115200, "Nova linha"):
//   30   -> força a temperatura em 30 °C
//   u90  -> força a umidade em 90 %
//   a    -> volta ao sensor (automático)
//   m3   -> toca o som 3 (mesmo que apertar o botão da página 3)
//   r    -> risada (ha-ha-ha)
//   v3   -> lentidão da boca (0.5 rápido ... 6 bem lento; padrão 4). "v" sozinho mostra o valor
//   f    -> fala (sílabas variadas)
//   s15  -> move o servo da boca para 15° (limitado à faixa segura 10 a 50°)
// Botões "Return Key Code" no DGUS: todos com VP 0x5100; Key Code = número da página (1 a 6).
// Cada botão toca mp3/000N.mp3 do cartão do DFPlayer (página 1 -> 0001.mp3 ... página 6 -> 0006.mp3).
// Enquanto o som toca, o servo SG90 mexe a boca do boneco.

#include <DHT.h>
#include <DFRobotDFPlayerMini.h>
#include <ESP32Servo.h>

#define DHT_PIN   4
#define DHT_TYPE  DHT11
#define DWIN_RX   16   // RX2 do ESP32 <- TX2 do display
#define DWIN_TX   17   // TX2 do ESP32 -> RX2 do display
#define MP3_RX    26   // RX1 do ESP32 <- TX do DFPlayer
#define MP3_TX    27   // TX1 do ESP32 -> RX do DFPlayer (com resistor de 1 kΩ)
#define SERVO_PIN 13   // sinal (fio laranja) do SG90

// Página do DGUS = número do BMP (1.bmp ... 6.bmp)
#define PAG_NUBLADO  1
#define PAG_FRESCO   2
#define PAG_FRIO     3
#define PAG_CHUVA    4
#define PAG_REGATA   5
#define PAG_BLUSA    6

// Faixas de temperatura, da mais fria para a mais quente (ponto médio entre os desenhos)
//   frio <15 | fresco 15-20 | blusa 21-23 | nublado 24-27 | regata >=28
const float   LIMITES[] = {15, 21, 24, 28};
const uint8_t PAGINAS[] = {PAG_FRIO, PAG_FRESCO, PAG_BLUSA, PAG_NUBLADO, PAG_REGATA};
const int     N_LIMITES = sizeof(LIMITES) / sizeof(LIMITES[0]);
const float   HISTERESE = 1.0;          // folga (°C) para a imagem não ficar piscando no limite

// Chuva: vale em qualquer temperatura
const float UMID_CHUVA = 85;            // entra na chuva a partir de 85 %
const float UMID_HIST  = 5;             // só sai da chuva abaixo de 80 %

#define VP_TEMP  0x6100                 // Data Variable no DGUS ("Temperatura do dia"), igual nas 6 páginas
#define VP_BOTAO 0x5100                 // Return Key Code no DGUS (o mesmo nos 6 botões)
#define N_SONS   6                      // Key Codes 1 a 6 -> mp3/0001.mp3 a mp3/0006.mp3

#define VOLUME_MP3    20                // 0 a 30

// Boca do boneco — calibre com o comando s<ângulo> no Monitor Serial
#define ANG_FECHADA   10                // ângulo com a boca fechada
#define ANG_ABERTA    50                // abertura máxima — NÃO passar disso (trava a tampa da boca)
// Lentidão da boca: multiplica todos os tempos. 1.0 = rápido, 2.2 = ~45 % da velocidade.
// Ajuste no Monitor Serial com v<valor> (ex.: v3). Volta a 4 quando o ESP32 reinicia.
float lentidao = 4.0;
const float LENTIDAO_MIN = 0.5;
const float LENTIDAO_MAX = 6.0;
#define BOCA_MIN_MS   (long)(90 * lentidao)    // tempo mínimo de cada movimento (ms)
#define BOCA_MAX_MS   (long)(200 * lentidao)   // tempo máximo de cada movimento (ms)
const unsigned long FALA_MAX_MS = 60000;      // segurança: para a boca se o aviso de fim não chegar
const unsigned long SOM_SIMULADO_MS = 3500;   // sem DFPlayer: a boca mexe por esse tempo (duração dos áudios provisórios)

const unsigned long INTERVALO_DHT = 2000;  // o DHT11 não aceita leituras mais rápidas que ~1 s

DHT dht(DHT_PIN, DHT_TYPE);
DFRobotDFPlayerMini mp3;
bool mp3Ok = false;
Servo boca;
bool falando = false;                   // true enquanto o som toca
bool bocaAberta = false;
bool servoLigado = false;
unsigned long inicioFala = 0;
unsigned long fimFala = 0;
unsigned long proximoMovimento = 0;
unsigned long fimSimulado = 0;          // != 0: som simulado (sem DFPlayer) termina nesse instante
bool debugDisplay = false;              // comando "d": mostra tudo o que chega do display
unsigned long ultimaConsulta = 0;
const unsigned long INTERVALO_BOTAO = 150;   // consulta o VP do botão a cada 150 ms
float tempAtual = 20;                   // última temperatura (sensor ou forçada)
float umidAtual = 50;                   // última umidade (sensor ou forçada)
int   faixaAtual = -1;                  // índice em PAGINAS; -1 = nenhuma ainda
bool  chovendo = false;
int   paginaAtual = -1;
bool  modoForcado = false;              // true = usa os valores digitados no Monitor Serial
unsigned long ultimaLeitura = 0;

void dwinPagina(uint8_t pagina) {
  uint8_t f[] = {0x5A, 0xA5, 0x07, 0x82, 0x00, 0x84, 0x5A, 0x01, 0x00, pagina};
  Serial2.write(f, sizeof(f));
}

void dwinEscreveVP(uint16_t vp, int16_t valor) {
  uint8_t f[] = {0x5A, 0xA5, 0x05, 0x82,
                 (uint8_t)(vp >> 8), (uint8_t)vp,
                 (uint8_t)(valor >> 8), (uint8_t)valor};
  Serial2.write(f, sizeof(f));
}

// Faixa "pura" da temperatura: 0 = frio ... N_LIMITES = regata
int faixaDe(float t) {
  int f = 0;
  while (f < N_LIMITES && t >= LIMITES[f]) f++;
  return f;
}

// Só troca de faixa se passar do limite com folga
int faixaComHisterese(float t, int atual) {
  int alvo = faixaDe(t);
  if (atual < 0 || alvo == atual) return alvo;
  if (alvo > atual && t >= LIMITES[atual] + HISTERESE)     return alvo;
  if (alvo < atual && t <  LIMITES[atual - 1] - HISTERESE) return alvo;
  return atual;
}

// Chuva com histerese: entra em 85 %, sai abaixo de 80 %
bool chuvaComHisterese(float h, bool agora) {
  return agora ? h >= UMID_CHUVA - UMID_HIST : h >= UMID_CHUVA;
}

// Decide a página e atualiza o display
void atualizar(bool comHisterese, const char *origem) {
  faixaAtual = comHisterese ? faixaComHisterese(tempAtual, faixaAtual) : faixaDe(tempAtual);
  chovendo   = comHisterese ? chuvaComHisterese(umidAtual, chovendo) : umidAtual >= UMID_CHUVA;

  int pagina = chovendo ? PAG_CHUVA : PAGINAS[faixaAtual];
  if (pagina != paginaAtual) {
    paginaAtual = pagina;
    dwinPagina(paginaAtual);
  }
  dwinEscreveVP(VP_TEMP, (int16_t)round(tempAtual));
  Serial.printf("[%s] Temp: %.1f C  Umid: %.0f %% -> pagina %d\n",
                origem, tempAtual, umidAtual, paginaAtual);
}

void servoLigar() {
  if (!servoLigado) {
    boca.attach(SERVO_PIN, 500, 2400);
    servoLigado = true;
  }
}

void bocaComecar() {
  servoLigar();
  falando = true;
  inicioFala = millis();
  proximoMovimento = 0;
}

void bocaParar() {
  falando = false;
  bocaAberta = false;
  boca.write(ANG_FECHADA);
  fimFala = millis();
}

// Abre e fecha a boca em tempos e aberturas variados, para parecer fala
void atualizarBoca() {
  if (!falando) {
    // desliga o servo pouco depois de fechar a boca, para ele não ficar zumbindo
    if (servoLigado && (long)(millis() - fimFala) > 400) {   // (long): fimFala pode estar no futuro
      boca.detach();
      servoLigado = false;
    }
    return;
  }
  if (millis() - inicioFala > FALA_MAX_MS) {
    bocaParar();
    return;
  }
  if (millis() >= proximoMovimento) {
    bocaAberta = !bocaAberta;
    int ang = bocaAberta ? map(random(50, 101), 0, 100, ANG_FECHADA, ANG_ABERTA) : ANG_FECHADA;
    boca.write(ang);
    proximoMovimento = millis() + random(BOCA_MIN_MS, BOCA_MAX_MS + 1);
  }
}

// Avisos do DFPlayer: quando o som termina, fecha a boca
void lerMp3() {
  if (fimSimulado && (long)(millis() - fimSimulado) >= 0) {
    fimSimulado = 0;
    Serial.println("Som simulado terminou");
    bocaParar();
  }
  if (!mp3Ok || !mp3.available()) return;
  if (mp3.readType() == DFPlayerPlayFinished && falando) {
    Serial.println("Som terminou");
    bocaParar();
  }
}

// Funções para risada e fala manual (podem usar 'boca' que já está declarado)
bool esperar(unsigned long ms) {
  ms = ms * lentidao;                   // risada e fala manual também ficam mais lentas
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    if (Serial.available()) return false;
    delay(2);
  }
  return true;
}

void abrir(float abertura) {
  int ang = round(ANG_FECHADA + abertura * (ANG_ABERTA - ANG_FECHADA));
  boca.write(constrain(ang, ANG_FECHADA, ANG_ABERTA));
}

bool silaba(float abertura, unsigned long msAberta, unsigned long msFechada) {
  abrir(abertura);
  if (!esperar(msAberta)) return false;
  abrir(random(0, 16) / 100.0);
  return esperar(msFechada);
}

void risadaManual() {
  Serial.println("Rindo...");
  falando = false;
  servoLigar();                         // o servo pode estar desligado (detach) para não zumbir
  for (int serie = 0; serie < 3; serie++) {
    int has = random(5, 8);
    for (int i = 0; i < has; i++) {
      float abertura = 1.0 - 0.08 * i;
      abrir(abertura);
      if (!esperar(90)) goto fim;
      abrir(0.1);
      if (!esperar(70)) goto fim;
    }
    abrir(0);
    if (!esperar(random(250, 450))) goto fim;
  }
  abrir(1.0);
  esperar(350);
fim:
  abrir(0);
  fimFala = millis();                   // desliga o servo um pouco depois de fechar
  Serial.println("Fim da risada");
}

void falaManual(unsigned long duracao) {
  Serial.println("Falando...");
  falando = false;
  servoLigar();
  unsigned long inicio = millis();
  int silabas = 0;
  int proximaPausa = random(3, 7);
  while (millis() - inicio < duracao) {
    if (!silaba(random(40, 101) / 100.0, random(80, 180), random(60, 140))) break;
    if (++silabas >= proximaPausa) {
      abrir(0);
      if (!esperar(random(200, 450))) break;
      silabas = 0;
      proximaPausa = random(3, 7);
    }
  }
  abrir(0);
  fimFala = millis();
  Serial.println("Fim da fala");
}

// Botão do display: o Key Code é o número do som (apertar de novo recomeça o som)
void botaoApertado(uint16_t codigo) {
  Serial.printf("Botao do display: codigo %d\n", codigo);
  if (codigo < 1 || codigo > N_SONS) {
    Serial.println("Codigo fora de 1 a 6: confira o Key Code no DGUS");
    return;
  }
  if (!mp3Ok) {
    // Sem DFPlayer: simula o som para testar botão e boneco
    Serial.printf("Simulando som %d (DFPlayer nao conectado)\n", codigo);
    bocaComecar();
    fimSimulado = millis() + SOM_SIMULADO_MS;
    return;
  }
  mp3.playMp3Folder(codigo);
  fimSimulado = 0;
  bocaComecar();
}

// Lê as mensagens do display: 5A A5 LEN + LEN bytes.
// Toque de botão: 5A A5 06 83 VPh VPl 01 VALh VALl. "OK" de escrita: 5A A5 03 82 4F 4B.
void lerDisplay() {
  static uint8_t buf[32];
  static int n = 0;

  while (Serial2.available()) {
    uint8_t b = Serial2.read();
    if (debugDisplay) {
      if (b == 0x5A) Serial.println();
      Serial.printf("%02X ", b);
    }

    if (n == 0 && b != 0x5A) continue;                 // espera o início do quadro
    if (n == 1 && b != 0xA5) { n = (b == 0x5A); continue; }
    buf[n++] = b;

    if (n < 3) continue;
    int total = 3 + buf[2];                            // cabeçalho + tamanho informado
    if (total > (int)sizeof(buf)) { n = 0; continue; } // quadro inválido
    if (n < total) continue;

    n = 0;
    if (buf[3] == 0x83 && buf[2] >= 6) {               // resposta de leitura/toque
      uint16_t vp    = (buf[4] << 8) | buf[5];
      uint16_t valor = (buf[7] << 8) | buf[8];
      if (vp == VP_BOTAO && valor != 0) {
        dwinEscreveVP(VP_BOTAO, 0);                    // zera para não repetir o mesmo toque
        botaoApertado(valor);
      }
    }
  }
}

// Pergunta ao display o valor do VP do botão (funciona mesmo com o auto upload desligado)
void consultarBotao() {
  if (millis() - ultimaConsulta < INTERVALO_BOTAO) return;
  ultimaConsulta = millis();
  uint8_t f[] = {0x5A, 0xA5, 0x04, 0x83, (uint8_t)(VP_BOTAO >> 8), (uint8_t)VP_BOTAO, 0x01};
  Serial2.write(f, sizeof(f));
}

// Lê comandos do Monitor Serial
void lerMonitorSerial() {
  if (!Serial.available()) return;

  String linha = Serial.readStringUntil('\n');
  linha.trim();
  linha.replace(',', '.');              // aceita "23,5" além de "23.5"
  if (linha.length() == 0) return;

  if (linha.equalsIgnoreCase("a")) {
    modoForcado = false;
    ultimaLeitura = 0;                  // lê o sensor imediatamente
    Serial.println("Modo AUTOMATICO (sensor DHT11)");
    return;
  }

  if (linha.charAt(0) == 'm' || linha.charAt(0) == 'M') {
    botaoApertado(linha.substring(1).toInt());
    return;
  }

  if (linha.charAt(0) == 'v' || linha.charAt(0) == 'V') {
    String valor = linha.substring(1);
    valor.trim();
    if (valor.length() > 0) lentidao = constrain(valor.toFloat(), LENTIDAO_MIN, LENTIDAO_MAX);
    Serial.printf("Lentidao da boca: %.1f (0.5 rapido ... 6 lento)\n", lentidao);
    return;
  }

  if (linha.equalsIgnoreCase("d")) {
    debugDisplay = !debugDisplay;
    Serial.println(debugDisplay ? "Diagnostico ON: toque no display e veja os bytes recebidos"
                                : "Diagnostico OFF");
    return;
  }

  if (linha.equalsIgnoreCase("r")) { risadaManual(); return; }
  if (linha.equalsIgnoreCase("f")) { falaManual(6000); return; }

  if (linha.charAt(0) == 's' || linha.charAt(0) == 'S') {
    int ang = constrain(linha.substring(1).toInt(),
                        min(ANG_FECHADA, ANG_ABERTA), max(ANG_FECHADA, ANG_ABERTA));
    falando = false;
    servoLigar();
    boca.write(ang);
    fimFala = millis() + 2000;          // segura o servo ligado por ~2 s
    Serial.printf("Servo da boca em %d graus\n", ang);
    return;
  }

  bool ehUmidade = (linha.charAt(0) == 'u' || linha.charAt(0) == 'U');
  String numero = ehUmidade ? linha.substring(1) : linha;
  numero.trim();

  char c = numero.charAt(0);
  if (numero.length() == 0 || (!isDigit(c) && c != '-')) {
    Serial.println("Comando invalido. Use: 30 (temperatura), u90 (umidade), a (automatico), m1 a m6 (sons), s15 (servo 0-50).");
    return;
  }

  if (ehUmidade) umidAtual = numero.toFloat();
  else           tempAtual = numero.toFloat();
  modoForcado = true;
  atualizar(false, "FORCADO");          // sem histerese: troca na hora para testar
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);
  Serial2.begin(115200, SERIAL_8N1, DWIN_RX, DWIN_TX);
  Serial1.begin(9600, SERIAL_8N1, MP3_RX, MP3_TX);
  dht.begin();
  delay(1500);   // o display (e o DFPlayer) demoram mais para ligar que o ESP32

  mp3Ok = mp3.begin(Serial1);
  if (mp3Ok) mp3.volume(VOLUME_MP3);
  Serial.println(mp3Ok ? "DFPlayer OK" : "DFPlayer nao encontrado");

  servoLigar();                         // começa com a boca fechada
  boca.write(ANG_FECHADA);
  fimFala = millis();
  randomSeed(esp_random());

  // A página 0 não tem imagem: mostra o nublado até a primeira leitura do sensor
  paginaAtual = PAG_NUBLADO;
  dwinPagina(paginaAtual);

  Serial.println("Comandos: 30 (temp), u90 (umidade), a (sensor), m1-m6 (som), r (risada), f (fala), s10-s50 (servo), v4 (lentidao).");
}

void loop() {
  lerDisplay();
  consultarBotao();
  lerMonitorSerial();
  lerMp3();
  atualizarBoca();

  if (modoForcado) return;              // sensor ignorado enquanto estiver forçado

  if (millis() - ultimaLeitura >= INTERVALO_DHT || ultimaLeitura == 0) {
    ultimaLeitura = millis();
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (isnan(t) || isnan(h)) {
      Serial.println("Falha ao ler o DHT11");
    } else {
      tempAtual = t;
      umidAtual = h;
      atualizar(true, "SENSOR");
    }
  }
}
