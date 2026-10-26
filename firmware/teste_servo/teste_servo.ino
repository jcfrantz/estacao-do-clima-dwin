// Teste do servo SG90 — digite o ângulo no Monitor Serial
// Monitor Serial: 115200 baud, "Nova linha". Digite um número de 0 a 180 e aperte Enter.
// Use para achar os ângulos de boca fechada e boca aberta do boneco.

#include <ESP32Servo.h>

#define SERVO_PIN 13   // sinal (fio laranja) do SG90

Servo servo;
int anguloAtual = 90;

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);

  servo.attach(SERVO_PIN, 500, 2400);
  servo.write(anguloAtual);

  Serial.println();
  Serial.println("Teste do servo SG90");
  Serial.println("Digite um angulo de 0 a 180 e aperte Enter.");
  Serial.printf("Angulo atual: %d graus\n", anguloAtual);
}

void loop() {
  if (!Serial.available()) return;

  String linha = Serial.readStringUntil('\n');
  linha.trim();
  if (linha.length() == 0) return;

  if (!isDigit(linha.charAt(0))) {
    Serial.println("Digite so o numero do angulo (0 a 180).");
    return;
  }

  int angulo = linha.toInt();
  if (angulo < 0 || angulo > 180) {
    Serial.println("Fora da faixa: use de 0 a 180.");
    return;
  }

  anguloAtual = angulo;
  servo.write(anguloAtual);
  Serial.printf("Servo em %d graus\n", anguloAtual);
}
