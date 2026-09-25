#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <qrcode.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Adafruit_NeoPixel.h>

// --- UUIDs PARA BLUETOOTH LOW ENERGY (BLE) ---
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID_RX "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// --- PINES E-PAPER (SPI) ---
#define EPD_BUSY 5
#define EPD_RST  2
#define EPD_DC   3
#define EPD_CS   7
#define EPD_SCK  4
#define EPD_MOSI 6

// --- PINES ENCODER Y SWITCH ---
#define ENCODER_CLK     20  // RX
#define ENCODER_DT      21  // TX
#define ENCODER_SW      0   // Click del Encoder
#define PIN_POMODORO_SW 9   // Switch Extra de Teclado

#ifdef PIN_NEOPIXEL
  #undef PIN_NEOPIXEL
#endif
#define PIN_NEOPIXEL 10
#define NUM_LEDS     12

Adafruit_NeoPixel anillo(NUM_LEDS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT> epaper(
    GxEPD2_290_C90c(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

Preferences preferences;

// Datos guardados
String wifiSSID = "";
String wifiPass = "";
String notionToken = "";
String notionDatabaseId = "";

bool datosRecibidosPorBLE = false;

struct Tarea {
  String idNotion;
  String titulo;
  String descripcion;
  bool completada;
  bool esConfiguracion;
};

const int TAREAS_POR_PAGINA = 4;
const int TOTAL_PAGINAS = 2;

Tarea tareasPagina1[TAREAS_POR_PAGINA];
Tarea tareasPagina2[TAREAS_POR_PAGINA];

enum EstadoSistema { 
  MODO_VINCULACION_BLE,
  MODO_MENU, 
  MODO_DETALLE_TAREA, 
  CONFIG_TRABAJO_RELOJ,
  CONFIG_DESCANSO_RELOJ,
  FOCUS_TRABAJO, 
  FOCUS_TRABAJO_ALERTA, 
  FOCUS_DESCANSO, 
  FOCUS_DESCANSO_ALERTA 
};

EstadoSistema estadoActual = MODO_MENU;

int paginaActual = 0;
int tareaSeleccionada = 0;

int minutosTrabajo = 25; 
int minutosDescanso = 5;

unsigned long tiempoInicioTimer = 0;
unsigned long duracionTimerMs = 0;

int ultimoCLK;
unsigned long ultimoClickEncoderMs = 0;
int conteoClicksEncoder = 0;

unsigned long tiempoPresionadoSwitch = 0;
bool switchPresionado = false;

unsigned long inicioPresionDobleMs = 0;
bool ambosBotonesPresionados = false;

// --- DIBUJO DE QR EN E-PAPER ---
void dibujarCodigoQR(String textoURL, int posX, int posY, int escala) {
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(3)];
  qrcode_initText(&qrcode, qrcodeData, 3, 0, textoURL.c_str());

  for (uint8_t y = 0; y < qrcode.size; y++) {
    for (uint8_t x = 0; x < qrcode.size; x++) {
      if (qrcode_getModule(&qrcode, x, y)) {
        epaper.fillRect(
          posX + (x * escala), 
          posY + (y * escala), 
          escala, 
          escala, 
          GxEPD_BLACK
        );
      }
    }
  }
}

void renderizarPantallaSetupBLE() {
  epaper.firstPage();
  do {
    epaper.fillScreen(GxEPD_WHITE);
    epaper.setFont(&FreeMonoBold9pt7b);
    
    epaper.setTextColor(GxEPD_RED);
    epaper.setCursor(10, 20);
    epaper.println("AuraTask BLE Setup");
    epaper.drawFastHLine(10, 26, 276, GxEPD_RED);

    epaper.setTextColor(GxEPD_BLACK);
    epaper.setCursor(10, 50);
    epaper.println("1. Escanea el QR");
    epaper.setCursor(10, 72);
    epaper.println("2. Conecta BLE");
    epaper.setCursor(10, 94);
    epaper.println("3. Guarda datos");

    // QR que apunta a la Web App de vinculación BLE
    dibujarCodigoQR("https://github.com/ESB7-ghost/AuraTask_OS", 185, 38, 2);

  } while (epaper.nextPage());

  epaper.powerOff();
}

// --- CALLBACKS PARA RECEPCIÓN DE DATOS BLE ---
class BLECallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      String jsonRx = pCharacteristic->getValue().c_str();

      if (jsonRx.length() > 0) {
        Serial.println("::: Datos recibidos via BLE :::");

        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, jsonRx);

        if (!error) {
          wifiSSID = doc["ssid"].as<String>();
          wifiPass = doc["pass"].as<String>();
          notionToken = doc["token"].as<String>();
          notionDatabaseId = doc["db_id"].as<String>();

          // Guardar en Memoria Flash NVS
          preferences.begin("auratask", false);
          preferences.putString("ssid", wifiSSID);
          preferences.putString("pass", wifiPass);
          preferences.putString("token", notionToken);
          preferences.putString("db_id", notionDatabaseId);
          preferences.end();

          datosRecibidosPorBLE = true;
        }
      }
    }
};

void iniciarServidorBLE() {
  Serial.println(">>> Iniciando Servidor BLE AuraTask-OS...");
  
  BLEDevice::init("AuraTask-OS");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic *pCharacteristic = pService->createCharacteristic(
                                         CHARACTERISTIC_UUID_RX,
                                         BLECharacteristic::PROPERTY_WRITE
                                       );

  pCharacteristic->setCallbacks(new BLECallbacks());
  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  // Anillo LED en Azul Neón esperando conexión
  anillo.fill(anillo.Color(0, 180, 255));
  anillo.show();

  renderizarPantallaSetupBLE();

  // Bucle de espera hasta recibir los datos por Bluetooth
  while (!datosRecibidosPorBLE) {
    delay(200);
  }

  // Confirmación visual
  anillo.fill(anillo.Color(0, 255, 0));
  anillo.show();
  delay(1000);

  BLEDevice::deinit(true); // Apagar Bluetooth para ahorrar energía
}

void ejecutarResetFactory() {
  Serial.println("\n!!! RESET DE FÁBRICA !!!");

  for (int i = 0; i < 4; i++) {
    anillo.fill(anillo.Color(255, 0, 40));
    anillo.show();
    delay(100);
    anillo.clear();
    anillo.show();
    delay(100);
  }

  preferences.begin("auratask", false);
  preferences.clear();
  preferences.end();

  ESP.restart();
}

void inicializarConexionYOnboarding() {
  preferences.begin("auratask", false);
  wifiSSID = preferences.getString("ssid", "");
  wifiPass = preferences.getString("pass", "");
  notionToken = preferences.getString("token", "");
  notionDatabaseId = preferences.getString("db_id", "");
  preferences.end();

  // Si no hay datos guardados, inicia el modo BLE con QR
  if (wifiSSID == "" || notionToken == "") {
    estadoActual = MODO_VINCULACION_BLE;
    iniciarServidorBLE();
  }

  // Conectar a Wi-Fi
  Serial.print("Conectando a Wi-Fi: ");
  Serial.println(wifiSSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 20) {
    delay(300);
    intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("::: Wi-Fi Conectado exitosamente :::");
  } else {
    Serial.println("::: Falla de conexión Wi-Fi :::");
  }
}

// --- PETICIONES HTTP NOTION ---
void sincronizarTareasDesdeNotion() {
  if (WiFi.status() != WL_CONNECTED || notionToken == "") {
    tareasPagina1[0] = {"", "1. Configurar Tiempos", "Ajustar reloj Pomodoro.", false, true};
    tareasPagina1[1] = {"", "2. Disenar AuraTask", "UI e-Paper y flujo.", false, false};
    tareasPagina1[2] = {"", "3. Probar Anillo LED", "Alimentacion 5V GPIO10.", false, false};
    tareasPagina1[3] = {"", "4. Conectar Notion", "Vincular via API.", false, false};
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://api.notion.com/v1/databases/" + notionDatabaseId + "/query";

  http.begin(client, url);
  http.addHeader("Authorization", "Bearer " + notionToken);
  http.addHeader("Notion-Version", "2022-06-28");
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST("{}");

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(8192);
    deserializeJson(doc, payload);
    JsonArray results = doc["results"].as<JsonArray>();

    tareasPagina1[0] = {"", "1. Configurar Tiempos", "Ajustar reloj Pomodoro.", false, true};

    int idxGlobal = 1;
    for (JsonObject result : results) {
      if (idxGlobal >= 7) break;

      String id = result["id"].as<String>();
      String titulo = result["properties"]["Name"]["title"][0]["text"]["content"].as<String>();
      String desc = result["properties"]["Description"]["rich_text"][0]["text"]["content"].as<String>();
      bool done = result["properties"]["Done"]["checkbox"].as<bool>();

      if (desc == "") desc = "Sin descripcion.";

      if (idxGlobal < 4) {
        tareasPagina1[idxGlobal] = {id, String(idxGlobal + 1) + ". " + titulo, desc, done, false};
      } else {
        tareasPagina2[idxGlobal - 4] = {id, String(idxGlobal + 1) + ". " + titulo, desc, done, false};
      }
      idxGlobal++;
    }
  }
  http.end();
}

void marcarTareaCompletadaEnNotion(String idNotion) {
  if (WiFi.status() != WL_CONNECTED || idNotion == "") return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://api.notion.com/v1/pages/" + idNotion;

  http.begin(client, url);
  http.addHeader("Authorization", "Bearer " + notionToken);
  http.addHeader("Notion-Version", "2022-06-28");
  http.addHeader("Content-Type", "application/json");

  String body = "{\"properties\":{\"Done\":{\"checkbox\":true}}}";
  http.PATCH(body);
  http.end();
}

// --- ANIMACIONES & RENDERIZADO ---
uint32_t obtenerColorPosicion(int pagina, int posicion) {
  if (pagina == 0 && posicion == 0) return anillo.Color(255, 0, 40);
  if (pagina == 0) {
    switch (posicion) {
      case 1: return anillo.Color(0, 180, 255);
      case 2: return anillo.Color(0, 230, 100);
      case 3: return anillo.Color(255, 140, 0);
    }
  } else {
    switch (posicion) {
      case 0: return anillo.Color(180, 0, 255);
      case 1: return anillo.Color(255, 20, 140);
      case 2: return anillo.Color(0, 80, 255);
      case 3: return anillo.Color(140, 255, 0);
    }
  }
  return anillo.Color(0, 150, 255);
}

void animacionRespiroTransicion(uint32_t colorBase) {
  for (int b = 10; b <= 200; b += 8) {
    uint8_t r = (uint8_t)(((colorBase >> 16) & 0xFF) * (b / 200.0));
    uint8_t g = (uint8_t)(((colorBase >> 8) & 0xFF) * (b / 200.0));
    uint8_t blue = (uint8_t)((colorBase & 0xFF) * (b / 200.0));
    anillo.fill(anillo.Color(r, g, blue));
    anillo.show();
    delay(10);
  }
}

void animacionZenBreatheAlerta(uint32_t colorHue) {
  float angulo = (millis() % 2500) / 2500.0 * 2 * PI;
  int brillo = (sin(angulo) + 1.0) * 100 + 15;
  uint8_t r = (uint8_t)(((colorHue >> 16) & 0xFF) * (brillo / 230.0));
  uint8_t g = (uint8_t)(((colorHue >> 8) & 0xFF) * (brillo / 230.0));
  uint8_t b = (uint8_t)((colorHue & 0xFF) * (brillo / 230.0));
  anillo.fill(anillo.Color(r, g, b));
  anillo.show();
}

void efectoFuegoDiscreto(int ledsEncendidos) {
  anillo.clear();
  unsigned long ms = millis();
  for (int i = 0; i < ledsEncendidos; i++) {
    float onda1 = sin((ms / 300.0) + (i * 0.8));
    float onda2 = sin((ms / 220.0) - (i * 0.9));
    float mezclado = (onda1 + onda2 + 2.0) / 4.0; 
    uint8_t r = 200 + (mezclado * 55);
    uint8_t g = (mezclado * 90);
    anillo.setPixelColor(i, anillo.Color(r, g, 0));
  }
  anillo.show();
}

void efectoAguaDiscreta(int ledsEncendidos) {
  anillo.clear();
  unsigned long ms = millis();
  for (int i = 0; i < ledsEncendidos; i++) {
    float onda1 = sin((ms / 350.0) + (i * 0.8));
    float onda2 = sin((ms / 250.0) - (i * 0.7));
    float mezclado = (onda1 + onda2 + 2.0) / 4.0;
    uint8_t g = mezclado * 110;        
    uint8_t b = 150 + (mezclado * 105);   
    anillo.setPixelColor(i, anillo.Color(0, g, b));
  }
  anillo.show();
}

void feedbackCambioPagina() {
  for (int i = 0; i < NUM_LEDS; i++) {
    anillo.setPixelColor(i, anillo.Color(255, 200, 0));
    anillo.show();
    delay(15);
  }
  delay(60);
  anillo.clear();
  anillo.show();
}

void actualizarAnilloRelojColor(int minutos, uint32_t colorRelleno) {
  anillo.clear();
  anillo.setPixelColor(0, anillo.Color(0, 255, 0));
  int pasoReloj = minutos / 5;
  for (int i = 1; i <= pasoReloj && i < NUM_LEDS; i++) {
    anillo.setPixelColor(i, colorRelleno);
  }
  anillo.show();
}

void actualizarAnilloSeleccion(int indice) {
  anillo.clear();
  int ledsPorTarea = NUM_LEDS / TAREAS_POR_PAGINA;
  int inicio = indice * ledsPorTarea;
  uint32_t colorSeleccion = obtenerColorPosicion(paginaActual, indice);
  for (int i = inicio; i < inicio + ledsPorTarea; i++) {
    anillo.setPixelColor(i, colorSeleccion);
  }
  anillo.show();
}

void actualizarProgresoAnillo() {
  unsigned long tiempoTranscurrido = millis() - tiempoInicioTimer;
  if (tiempoTranscurrido >= duracionTimerMs) {
    if (estadoActual == FOCUS_TRABAJO) {
      estadoActual = FOCUS_TRABAJO_ALERTA;
    } else if (estadoActual == FOCUS_DESCANSO) {
      estadoActual = FOCUS_DESCANSO_ALERTA;
    }
    return;
  }
  float porcentajeRestante = 1.0 - ((float)tiempoTranscurrido / (float)duracionTimerMs);
  int ledsEncendidos = round(porcentajeRestante * NUM_LEDS);

  if (estadoActual == FOCUS_TRABAJO) {
    efectoFuegoDiscreto(ledsEncendidos);
  } else if (estadoActual == FOCUS_DESCANSO) {
    efectoAguaDiscreta(ledsEncendidos);
  }
}

void renderizarEPaper() {
  epaper.firstPage();
  do {
    epaper.fillScreen(GxEPD_WHITE);
    epaper.setFont(&FreeMonoBold9pt7b);
    
    Tarea* listaActual = (paginaActual == 0) ? tareasPagina1 : tareasPagina2;

    if (estadoActual == MODO_MENU) {
      epaper.setTextColor(GxEPD_BLACK);
      epaper.setCursor(10, 22);
      epaper.printf("AuraTask OS (Pag %d/%d)\n", paginaActual + 1, TOTAL_PAGINAS);
      epaper.drawFastHLine(10, 28, 276, GxEPD_RED);

      for (int i = 0; i < TAREAS_POR_PAGINA; i++) {
        int posY = 52 + (i * 22);
        if (i == tareaSeleccionada) {
          epaper.setTextColor(GxEPD_RED);
          epaper.setCursor(10, posY);
          epaper.print("> ");
          epaper.println(listaActual[i].titulo);
        } else {
          epaper.setTextColor(listaActual[i].completada ? GxEPD_RED : GxEPD_BLACK);
          epaper.setCursor(25, posY);
          epaper.println(listaActual[i].titulo + (listaActual[i].completada ? " [DONE]" : ""));
        }

        if (listaActual[i].completada) {
          int anchoTexto = listaActual[i].titulo.length() * 11; 
          epaper.drawFastHLine(25, posY - 5, anchoTexto, GxEPD_RED);
        }
      }
    } 
    else if (estadoActual == MODO_DETALLE_TAREA) {
      epaper.setTextColor(GxEPD_BLACK);
      epaper.setCursor(10, 22);
      epaper.println("Detalle de Tarea");
      epaper.drawFastHLine(10, 28, 276, GxEPD_BLACK);

      epaper.setTextColor(GxEPD_RED);
      epaper.setCursor(10, 48);
      epaper.println(listaActual[tareaSeleccionada].titulo);

      epaper.setTextColor(GxEPD_BLACK);
      epaper.setCursor(10, 72);
      epaper.println(listaActual[tareaSeleccionada].descripcion);

      epaper.setTextColor(GxEPD_RED);
      epaper.setCursor(10, 102);
      epaper.printf("Timer: %dm Focus / %dm Break", minutosTrabajo, minutosDescanso);
    } 
    else if (estadoActual == CONFIG_TRABAJO_RELOJ) {
      epaper.setTextColor(GxEPD_RED);
      epaper.setCursor(10, 22);
      epaper.println("1/2 TIEMPO TRABAJO");
      epaper.drawFastHLine(10, 28, 276, GxEPD_RED);

      epaper.setTextColor(GxEPD_BLACK);
      epaper.setCursor(10, 58);
      epaper.printf("Focus: %d min\n", minutosTrabajo);
      epaper.setCursor(10, 88);
      epaper.println("Switch -> Confirmar");
    }
    else if (estadoActual == CONFIG_DESCANSO_RELOJ) {
      epaper.setTextColor(GxEPD_RED);
      epaper.setCursor(10, 22);
      epaper.println("2/2 TIEMPO DESCANSO");
      epaper.drawFastHLine(10, 28, 276, GxEPD_RED);

      epaper.setTextColor(GxEPD_BLACK);
      epaper.setCursor(10, 58);
      epaper.printf("Break: %d min\n", minutosDescanso);
      epaper.setCursor(10, 88);
      epaper.println("Switch -> Guardar");
    }
    else {
      epaper.setTextColor(GxEPD_RED);
      epaper.setCursor(10, 22);
      epaper.println("WORKING ON IT...");
      epaper.drawFastHLine(10, 28, 276, GxEPD_RED);

      epaper.setTextColor(GxEPD_RED);
      epaper.setCursor(10, 54);
      epaper.print("TAREA: ");
      epaper.println(listaActual[tareaSeleccionada].titulo);

      epaper.setTextColor(GxEPD_BLACK);
      epaper.setCursor(10, 82);
      epaper.println(listaActual[tareaSeleccionada].descripcion);
    }

  } while (epaper.nextPage());

  epaper.powerOff();
}

void iniciarBloqueTimer(EstadoSistema nuevoEstado, int minutos, bool refrescarPantalla) {
  estadoActual = nuevoEstado;
  tiempoInicioTimer = millis();
  duracionTimerMs = (unsigned long)minutos * 60 * 1000;
  if (refrescarPantalla) renderizarEPaper();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(EPD_BUSY, INPUT_PULLUP);

  anillo.begin();
  anillo.setBrightness(45);
  anillo.clear();
  anillo.show();

  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT, INPUT_PULLUP);
  pinMode(ENCODER_SW, INPUT_PULLUP);
  pinMode(PIN_POMODORO_SW, INPUT_PULLUP);

  ultimoCLK = digitalRead(ENCODER_CLK);

  SPI.end();
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  epaper.init(115200, true, 50, false);
  epaper.setRotation(1);

  // INICIALIZA CONEXIÓN BLE / WI-FI
  inicializarConexionYOnboarding();

  sincronizarTareasDesdeNotion();
  estadoActual = MODO_MENU;
  actualizarAnilloSeleccion(tareaSeleccionada);
  renderizarEPaper();
}

void loop() {
  // RESET DE FÁBRICA CON DOBLE BOTÓN POR 5 SEGUNDOS
  if (digitalRead(ENCODER_SW) == LOW && digitalRead(PIN_POMODORO_SW) == LOW) {
    if (!ambosBotonesPresionados) {
      ambosBotonesPresionados = true;
      inicioPresionDobleMs = millis();
    } else {
      if (millis() - inicioPresionDobleMs >= 5000) {
        ejecutarResetFactory();
      }
    }
  } else {
    ambosBotonesPresionados = false;
  }

  if (estadoActual == FOCUS_TRABAJO || estadoActual == FOCUS_DESCANSO) {
    actualizarProgresoAnillo();
  } 
  else if (estadoActual == FOCUS_TRABAJO_ALERTA) {
    animacionZenBreatheAlerta(anillo.Color(255, 40, 0));
  } 
  else if (estadoActual == FOCUS_DESCANSO_ALERTA) {
    animacionZenBreatheAlerta(anillo.Color(0, 200, 255));
  }

  // GIRO ENCODER
  int estadoCLK = digitalRead(ENCODER_CLK);
  if (estadoCLK != ultimoCLK && estadoCLK == LOW) {
    if (estadoActual == MODO_MENU) {
      if (digitalRead(ENCODER_DT) != estadoCLK) {
        tareaSeleccionada = (tareaSeleccionada + 1) % TAREAS_POR_PAGINA;
      } else {
        tareaSeleccionada = (tareaSeleccionada - 1 + TAREAS_POR_PAGINA) % TAREAS_POR_PAGINA;
      }
      actualizarAnilloSeleccion(tareaSeleccionada);
    } 
    else if (estadoActual == CONFIG_TRABAJO_RELOJ) {
      if (digitalRead(ENCODER_DT) != estadoCLK) {
        minutosTrabajo = (minutosTrabajo + 5) % 60;
        if (minutosTrabajo == 0) minutosTrabajo = 55;
      } else {
        minutosTrabajo = (minutosTrabajo - 5 + 55) % 60;
        if (minutosTrabajo <= 0) minutosTrabajo = 55;
      }
      actualizarAnilloRelojColor(minutosTrabajo, anillo.Color(255, 40, 0));
    }
    else if (estadoActual == CONFIG_DESCANSO_RELOJ) {
      if (digitalRead(ENCODER_DT) != estadoCLK) {
        minutosDescanso = (minutosDescanso + 5) % 60;
        if (minutosDescanso == 0) minutosDescanso = 55;
      } else {
        minutosDescanso = (minutosDescanso - 5 + 55) % 60;
        if (minutosDescanso <= 0) minutosDescanso = 55;
      }
      actualizarAnilloRelojColor(minutosDescanso, anillo.Color(0, 200, 200));
    }
  }
  ultimoCLK = estadoCLK;

  // CLICK ENCODER
  int lecturaEncoderSW = digitalRead(ENCODER_SW);
  if (lecturaEncoderSW == LOW) {
    if (millis() - ultimoClickEncoderMs > 250) {
      conteoClicksEncoder++;
      ultimoClickEncoderMs = millis();
    }
  }

  if (conteoClicksEncoder > 0 && (millis() - ultimoClickEncoderMs > 300)) {
    if (conteoClicksEncoder == 1) {
      if (estadoActual == MODO_MENU) {
        animacionRespiroTransicion(anillo.Color(0, 255, 0));
        estadoActual = MODO_DETALLE_TAREA;
        renderizarEPaper();
      } else if (estadoActual == MODO_DETALLE_TAREA) {
        animacionRespiroTransicion(anillo.Color(0, 255, 0));
        estadoActual = MODO_MENU;
        actualizarAnilloSeleccion(tareaSeleccionada);
        renderizarEPaper();
      } else {
        animacionRespiroTransicion(anillo.Color(0, 255, 0));
        estadoActual = MODO_MENU;
        actualizarAnilloSeleccion(tareaSeleccionada);
        renderizarEPaper();
      }
    } 
    else if (conteoClicksEncoder >= 2) {
      if (estadoActual == MODO_MENU) {
        paginaActual = (paginaActual + 1) % TOTAL_PAGINAS;
        tareaSeleccionada = 0;
        feedbackCambioPagina();
        actualizarAnilloSeleccion(tareaSeleccionada);
        renderizarEPaper();
      }
    }
    conteoClicksEncoder = 0;
  }

  // SWITCH POMODORO
  int lecturaSwitch = digitalRead(PIN_POMODORO_SW);
  if (lecturaSwitch == LOW && !switchPresionado) {
    switchPresionado = true;
    tiempoPresionadoSwitch = millis();
  }
  if (lecturaSwitch == HIGH && switchPresionado) {
    unsigned long duracionClick = millis() - tiempoPresionadoSwitch;
    switchPresionado = false;

    Tarea* listaActual = (paginaActual == 0) ? tareasPagina1 : tareasPagina2;

    if (duracionClick > 1200) {
      if (estadoActual != MODO_MENU && estadoActual != MODO_DETALLE_TAREA && estadoActual != CONFIG_TRABAJO_RELOJ && estadoActual != CONFIG_DESCANSO_RELOJ) {
        animacionRespiroTransicion(anillo.Color(0, 255, 0));
        listaActual[tareaSeleccionada].completada = true;
        
        marcarTareaCompletadaEnNotion(listaActual[tareaSeleccionada].idNotion);

        estadoActual = MODO_MENU;
        actualizarAnilloSeleccion(tareaSeleccionada);
        renderizarEPaper();
      }
    } else if (duracionClick > 50) {
      if (estadoActual == MODO_MENU || estadoActual == MODO_DETALLE_TAREA) {
        if (listaActual[tareaSeleccionada].esConfiguracion) {
          animacionRespiroTransicion(anillo.Color(255, 0, 40));
          estadoActual = CONFIG_TRABAJO_RELOJ;
          actualizarAnilloRelojColor(minutosTrabajo, anillo.Color(255, 40, 0));
          renderizarEPaper();
        } else {
          animacionRespiroTransicion(anillo.Color(255, 40, 0));
          iniciarBloqueTimer(FOCUS_TRABAJO, minutosTrabajo, true);
        }
      } 
      else if (estadoActual == CONFIG_TRABAJO_RELOJ) {
        animacionRespiroTransicion(anillo.Color(0, 200, 200));
        estadoActual = CONFIG_DESCANSO_RELOJ;
        actualizarAnilloRelojColor(minutosDescanso, anillo.Color(0, 200, 200));
        renderizarEPaper();
      }
      else if (estadoActual == CONFIG_DESCANSO_RELOJ) {
        animacionRespiroTransicion(anillo.Color(0, 255, 0));
        estadoActual = MODO_MENU;
        actualizarAnilloSeleccion(tareaSeleccionada);
        renderizarEPaper();
      }
      else if (estadoActual == FOCUS_TRABAJO || estadoActual == FOCUS_TRABAJO_ALERTA) {
        animacionRespiroTransicion(anillo.Color(0, 200, 200));
        iniciarBloqueTimer(FOCUS_DESCANSO, minutosDescanso, false);
      } 
      else if (estadoActual == FOCUS_DESCANSO || estadoActual == FOCUS_DESCANSO_ALERTA) {
        animacionRespiroTransicion(anillo.Color(255, 40, 0));
        iniciarBloqueTimer(FOCUS_TRABAJO, minutosTrabajo, false);
      }
    }
  }
}