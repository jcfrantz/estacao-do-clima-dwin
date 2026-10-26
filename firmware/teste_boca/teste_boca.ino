// Teste da boca do boneco — fala e risada com o servo SG90
// Monitor Serial: 115200 baud, "Nova linha".
//   f      -> fala (6 s)
//   r      -> risada (ha-ha-ha)
//   0..50  -> move a boca para esse ângulo
//   qualquer tecla durante a fala/risada interrompe
//
// SEGURANÇA: o servo nunca sai da faixa BOCA_FECHADA..BOCA_ABERTA (0 a 50°),
// para não travar a tampa da boca.

#include <ESP32Servo.h>

#define SERVO_PIN    13   // sinal (fio laranja) do SG90
#define BOCA_FECHADA 0    // ângulo com a boca fechada
#define BOCA_ABERTA  50   // ângulo máximo de abertura — NÃO passar disso

const int ANG_MIN = min(BOCA_FECHADA, BOCA_ABERTA);
const int ANG_MAX = max(BOCA_FECHADA, BOCA_ABERTA);

Servo boca;

// Único ponto que mexe no servo: sempre limita à faixa segura
void moverBoca(int angulo) {
  boca.write(constrain(angulo, ANG_MIN, ANG_MAX));
}

// Abertura de 0.0 (fechada) a 1.0 (toda aberta) -> ângulo
void abrir(float abertura) {
  moverBoca(round(BOCA_FECHADA + abertura * (BOCA_ABERTA - BOCA_FECHADA)));
}

// Espera sem travar; retorna false se chegou algo no Monitor Serial (interrompe)
bool esperar(unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    if (Serial.available()) return false;
    delay(2);
  }
  return true;
}

// Uma "sílaba": abre, espera, quase fecha, espera
bool silaba(float abertura, unsigned long msAberta, unsigned long msFechada) {
  abrir(abertura);
  if (!esperar(msAberta)) return false;
  abrir(random(0, 16) / 100.0);         // na fala a boca não fecha totalmente
  return esperar(msFechada);
}

// Fala: sílabas com abertura e tempo variados, com pausas entre "palavras"
void fala(unsigned long duracao) {
  Serial.println("Falando...");
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
  Serial.println("Fim da fala");
}

// Risada: séries de "ha" rápidos, cada um um pouco menor, com respiração entre as séries
void risada() {
  Serial.println("Rindo...");
  for (int serie = 0; serie < 3; serie++) {
    int has = random(5, 8);
    for (int i = 0; i < has; i++) {
      float abertura = 1.0 - 0.08 * i;  // "HA ha ha ha..." vai diminuindo
      abrir(abertura);
      if (!esperar(90)) goto fim;
      abrir(0.1);
      if (!esperar(70)) goto fim;
    }
    abrir(0);
    if (!esperar(random(250, 450))) goto fim;
  }
  // "haaaa" final: abre tudo, segura e fecha
  abrir(1.0);
  esperar(350);
fim:
  abrir(0);
  Serial.println("Fim da risada");
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);
  randomSeed(esp_random());

  boca.attach(SERVO_PIN, 500, 2400);
  moverBoca(BOCA_FECHADA);              // primeiro pulso já é boca fechada

  Serial.println();
  Serial.printf("Teste da boca (faixa segura %d a %d graus)\n", ANG_MIN, ANG_MAX);
  Serial.printf("Comandos: f = fala | r = risada | %d a %d = angulo\n", ANG_MIN, ANG_MAX);
}

void loop() {
  if (!Serial.available()) return;

  String linha = Serial.readStringUntil('\n');
  linha.trim();
  if (linha.length() == 0) return;

  char c = tolower(linha.charAt(0));
  if (c == 'f') { fala(6000); return; }
  if (c == 'r') { risada(); return; }

  if (isDigit(c)) {
    int ang = linha.toInt();
    if (ang < ANG_MIN || ang > ANG_MAX) {
      Serial.printf("Fora da faixa segura: use de %d a %d.\n", ANG_MIN, ANG_MAX);
      return;
    }
    moverBoca(ang);
    Serial.printf("Boca em %d graus\n", ang);
    return;
  }

  Serial.printf("Comandos: f = fala | r = risada | %d a %d = angulo\n", ANG_MIN, ANG_MAX);
}
