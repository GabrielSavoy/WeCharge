/*
  ============================================================
  WeCharge / ChargeGrid — Maquete Física (Arduino UNO)
  INTEGRACAO ROBUSTA COM SITE + RFID + 2 BOTOES + BH1750
  ============================================================

  FLUXO:
  1) Site conecta pela USB (Web Serial) e envia HELLO.
  2) Arduino responde EVENTO:PRONTO.
  3) Cartao RFID -> Arduino envia EVENTO:RFID_OK.
  4) Site libera automaticamente a etapa de escolha.
  5) Site envia 'U' ou 'E'.
     U -> LED amarelo (URGENTE)
     E -> BH1750 decide:
          luz > limiar -> LED verde (SOLAR)
          luz <= limiar -> LED vermelho (REDE)
  6) Arduino confirma o resultado pela USB.

  BOTOES FISICOS (tambem funcionam):
  - pino 2 -> URGENTE -> LED amarelo
  - pino 3 -> ECONOMIZAR -> verde/vermelho conforme BH1750

  LIGACOES:
  RFID RC522: SDA->10, SCK->13, MOSI->11, MISO->12, RST->9, 3.3V, GND
  BH1750: SDA->A4, SCL->A5, VCC->5V, GND->GND
  LCD I2C: SDA->A4, SCL->A5, 5V, GND
  Botao urgente: pino 2 -> botao -> GND
  Botao economizar: pino 3 -> botao -> GND
  LED verde: pino 5 -> resistor 220R -> LED -> GND
  LED vermelho: pino 6 -> resistor 220R -> LED -> GND
  LED amarelo: pino 7 -> resistor 220R -> LED -> GND
  ============================================================
*/

#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <BH1750.h>

// ---------- Pinos ----------
#define RFID_SS_PIN       10
#define RFID_RST_PIN       9
#define BOTAO_URGENTE      2
#define BOTAO_ECONOMIZAR   3
#define LED_SOLAR          5   // verde
#define LED_REDE           6   // vermelho
#define LED_URGENTE        7   // amarelo

// ---------- Objetos ----------
MFRC522 leitorRFID(RFID_SS_PIN, RFID_RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);
BH1750 sensorLuz;

// ---------- Configuracao ----------
const float LIMIAR_LUZ = 20.0; // acima disso = SOLAR; cobrindo o sensor deve cair para REDE
const unsigned long DEBOUNCE_MS = 220;
const unsigned long AGUARDAR_ESCOLHA_MS = 15000;
const unsigned long TEMPO_LED_MS = 5000;

// ---------- Estado ----------
bool autenticado = false;
bool aguardandoEscolha = false;
unsigned long inicioAguardandoEscolha = 0;

unsigned long ultimoApertoUrgente = 0;
unsigned long ultimoApertoEconomizar = 0;
unsigned long desligarLedsEm = 0;
bool temporizadorLedAtivo = false;

String bufferSerial = "";

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(9600);
  delay(300);

  // RFID
  SPI.begin();
  leitorRFID.PCD_Init();

  // I2C
  Wire.begin();
  sensorLuz.begin();

  // Botoes
  pinMode(BOTAO_URGENTE, INPUT_PULLUP);
  pinMode(BOTAO_ECONOMIZAR, INPUT_PULLUP);

  // LEDs
  pinMode(LED_SOLAR, OUTPUT);
  pinMode(LED_REDE, OUTPUT);
  pinMode(LED_URGENTE, OUTPUT);
  apagarTodosLeds();
  cancelarTemporizadorLed();

  // LCD
  lcd.init();
  lcd.backlight();
  telaInicial();

  // Aviso inicial. O site tambem pode pedir HELLO depois.
  Serial.println("EVENTO:PRONTO");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  verificarSerial();
  atualizarLedTemporizado();

  if (!autenticado) {
    verificarCartao();
  } else if (aguardandoEscolha) {
    verificarBotoes();

    if (millis() - inicioAguardandoEscolha > AGUARDAR_ESCOLHA_MS) {
      Serial.println("EVENTO:TIMEOUT_ESCOLHA");
      resetarSistema();
    }
  }
}

// ============================================================
// SERIAL: SITE -> ARDUINO
// ============================================================
void verificarSerial() {
  while (Serial.available() > 0) {
    char c = Serial.read();

    // Site envia comandos simples separados por \n.
    if (c == '\n' || c == '\r') {
      if (bufferSerial.length() > 0) {
        bufferSerial.trim();
        processarComando(bufferSerial);
        bufferSerial = "";
      }
    } else if (c >= 32 && c <= 126) {
      bufferSerial += c;

      // Limite de seguranca
      if (bufferSerial.length() > 30) {
        bufferSerial = "";
      }
    }
  }
}

void processarComando(String comando) {
  comando.toUpperCase();

  if (comando == "HELLO" || comando == "PING") {
    Serial.println("EVENTO:PRONTO");
    return;
  }

  if (comando == "U") {
    Serial.println("EVENTO:RECEBI_U");
    processarUrgente(true);
    return;
  }

  if (comando == "E") {
    Serial.println("EVENTO:RECEBI_E");
    processarEconomizar(true);
    return;
  }

  // Reinicia a maquete junto com o botao "Reiniciar demonstracao" do site.
  if (comando == "RESET") {
    Serial.println("EVENTO:RESET_RECEBIDO");
    resetarSistema();
    Serial.println("EVENTO:RESET_OK");
    return;
  }
}

// ============================================================
// RFID
// ============================================================
void verificarCartao() {
  if (!leitorRFID.PICC_IsNewCardPresent()) {
    return;
  }

  if (!leitorRFID.PICC_ReadCardSerial()) {
    return;
  }

  autenticado = true;
  aguardandoEscolha = true;
  inicioAguardandoEscolha = millis();

  apagarTodosLeds();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Cartao OK!");
  lcd.setCursor(0, 1);
  lcd.print("Escolha no site");

  // Este e o evento que libera o site automaticamente.
  Serial.println("EVENTO:RFID_OK");

  leitorRFID.PICC_HaltA();
  leitorRFID.PCD_StopCrypto1();
}

// ============================================================
// BOTOES FISICOS
// ============================================================
void verificarBotoes() {
  if (digitalRead(BOTAO_URGENTE) == LOW) {
    if (millis() - ultimoApertoUrgente >= DEBOUNCE_MS) {
      ultimoApertoUrgente = millis();
      Serial.println("EVENTO:BOTAO_URGENTE");
      processarUrgente(false);
    }
    return;
  }

  if (digitalRead(BOTAO_ECONOMIZAR) == LOW) {
    if (millis() - ultimoApertoEconomizar >= DEBOUNCE_MS) {
      ultimoApertoEconomizar = millis();
      Serial.println("EVENTO:BOTAO_ECONOMIZAR");
      processarEconomizar(false);
    }
  }
}

// ============================================================
// MODO URGENTE
// ============================================================
void processarUrgente(bool veioDoSite) {
  autenticado = true;
  aguardandoEscolha = false;

  apagarTodosLeds();
  digitalWrite(LED_URGENTE, HIGH);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(veioDoSite ? "Site: URGENTE" : "Modo: URGENTE");
  lcd.setCursor(0, 1);
  lcd.print("R$ 1.79/kWh");

  Serial.println("EVENTO:URGENTE_OK");
  iniciarTemporizadorLeds();
}

// ============================================================
// MODO ECONOMIZAR
// ============================================================
void processarEconomizar(bool veioDoSite) {
  autenticado = true;
  aguardandoEscolha = false;

  apagarTodosLeds();

  float lux = sensorLuz.readLightLevel();

  // Se a biblioteca retornar valor negativo, considera erro do sensor.
  if (lux < 0) {
    Serial.println("EVENTO:BH1750_ERRO");
    digitalWrite(LED_REDE, HIGH);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Eco: REDE");
    lcd.setCursor(0, 1);
    lcd.print("Sensor indispon.");

    Serial.println("EVENTO:ECONOMIZAR_REDE");
    iniciarTemporizadorLeds();
    return;
  }

  Serial.print("EVENTO:LUX:");
  Serial.println(lux, 1);

  lcd.clear();
  lcd.setCursor(0, 0);

  if (lux > LIMIAR_LUZ) {
    // Tem luz suficiente -> SOLAR -> verde
    digitalWrite(LED_SOLAR, HIGH);

    lcd.print(veioDoSite ? "Site: SOLAR" : "Eco: SOLAR");
    lcd.setCursor(0, 1);
    lcd.print("R$ 0.89/kWh");

    Serial.println("EVENTO:ECONOMIZAR_SOLAR");
    iniciarTemporizadorLeds();
  } else {
    // Pouca luz -> REDE -> vermelho
    digitalWrite(LED_REDE, HIGH);

    lcd.print(veioDoSite ? "Site: REDE" : "Eco: REDE");
    lcd.setCursor(0, 1);
    lcd.print("R$ 1.42/kWh");

    Serial.println("EVENTO:ECONOMIZAR_REDE");
    iniciarTemporizadorLeds();
  }
}

// ============================================================
// TEMPORIZADOR DOS LEDs
// ============================================================
void iniciarTemporizadorLeds() {
  // Marca exatamente quando os LEDs devem apagar.
  // O timer fica independente da funcao apagarTodosLeds().
  desligarLedsEm = millis() + TEMPO_LED_MS;
  temporizadorLedAtivo = true;
}

void atualizarLedTemporizado() {
  if (!temporizadorLedAtivo) {
    return;
  }

  // Subtracao assinada tambem funciona quando millis() faz rollover.
  if ((long)(millis() - desligarLedsEm) >= 0) {
    digitalWrite(LED_SOLAR, LOW);
    digitalWrite(LED_REDE, LOW);
    digitalWrite(LED_URGENTE, LOW);

    temporizadorLedAtivo = false;
    desligarLedsEm = 0;

    // O efeito visual termina, mas a recarga continua.
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("CARREGANDO...");
    lcd.setCursor(0, 1);
    lcd.print("Recarga em curso");

    Serial.println("EVENTO:LEDS_OFF");
    Serial.println("EVENTO:CARREGANDO");
  }
}

// ============================================================
// UTILIDADES
// ============================================================
void apagarTodosLeds() {
  digitalWrite(LED_SOLAR, LOW);
  digitalWrite(LED_REDE, LOW);
  digitalWrite(LED_URGENTE, LOW);
}

void cancelarTemporizadorLed() {
  temporizadorLedAtivo = false;
  desligarLedsEm = 0;
}

void telaInicial() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WeCharge");
  lcd.setCursor(0, 1);
  lcd.print("Aproxime cartao");
}

void resetarSistema() {
  cancelarTemporizadorLed();
  apagarTodosLeds();
  autenticado = false;
  aguardandoEscolha = false;
  bufferSerial = "";
  leitorRFID.PICC_HaltA();
  leitorRFID.PCD_StopCrypto1();
  telaInicial();
}
