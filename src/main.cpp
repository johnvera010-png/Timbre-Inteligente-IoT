#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <ESP32Servo.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ESPAsyncWebServer.h>

// Credenciales WiFi
Preferences preferencias;
const char* ssid_default = "Prueba";
const char* password_default = "nosirve12";
String ssid_actual;
String password_actual;

// Definición de Pines
#define SDA_PIN 13
#define SCL_PIN 3
#define SERVO_PIN 4
#define TIMBRE_PIN 12

// Pines de la cámara OV2640
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

Servo cerradura;
Adafruit_PN532 nfc(SDA_PIN, SCL_PIN);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// UID llavero maestro
uint8_t uidAutorizado[] = { 0xC3, 0x45, 0x10, 0xF7 }; 

// Variables de Estado y Control
enum EstadoSistema { ESTADO_IDLE, ESTADO_RUN_ACCESO, ESTADO_RUN_ALERTA, ESTADO_WAIT, ESTADO_MANTENER_ABIERTA };
EstadoSistema estadoActual = ESTADO_IDLE;
unsigned long timerEstado = 0;
String ultimoUID = "N/A";
bool camaraLista = false;

// Prototipos de funciones
bool conectarWiFi(const char* ssid, const char* pass);
void inicializarCamara();
void inicializarSD();
void inicializarNFC();
void capturarYGuardarFoto();
void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);

// INTERFAZ WEB DEL DASHBOARD
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Dashboard - Timbre IoT</title>
  <style>
    body { font-family: Arial, sans-serif; background-color: #f4f4f9; text-align: center; margin: 0; padding: 20px; }
    .card { background: white; padding: 20px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.2); max-width: 400px; margin: 0 auto; }
    h2 { color: #333; }
    .btn { padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin: 5px; color: white; width: 45%; }
    .btn-green { background-color: #28a745; }
    .btn-red { background-color: #dc3545; }
    .btn-blue { background-color: #007bff; }
    .btn-gray { background-color: #6c757d; }
    #alerta-container { display: none; background: #fff3cd; border: 1px solid #ffeeba; padding: 15px; border-radius: 10px; margin-top: 20px; }
    img { max-width: 100%; border-radius: 5px; margin-bottom: 10px; }
    .step2 { display: none; }
    .manual-control { margin-top: 20px; padding-top: 20px; border-top: 1px solid #ddd; }
    .pin-input { padding: 10px; font-size: 16px; text-align: center; width: 120px; border: 1px solid #ccc; border-radius: 5px; margin-bottom: 10px; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Timbre Inteligente IoT</h2>
    <p>Estado Red: <span id="ws-status" style="color: green;">Conectando...</span></p>
    
    <div id="alerta-container">
      <h3 style="color: #dc3545;">¡ALERTA DE INTRUSO!</h3>
      <p>UID: <strong id="uid-text">--</strong></p>
      <img id="foto-intruso" src="" alt="Captura de Intruso">
      
      <div id="step1">
        <button class="btn btn-green" onclick="enviarComando('PERMITIR')">Permitir Acceso</button>
        <button class="btn btn-red" onclick="enviarComando('DENEGAR')">Denegar</button>
      </div>
      <div id="step2" class="step2">
        <p>¿Qué deseas hacer con la captura?</p>
        <button class="btn btn-blue" onclick="enviarComando('CONSERVAR')">Conservar</button>
        <button class="btn btn-gray" onclick="enviarComando('BORRAR')">Borrar (Liberar SD)</button>
      </div>
    </div>

    <!-- CONTROL MANUAL CON CONTRASEÑA -->
    <div class="manual-control">
      <h4 style="margin-bottom: 10px; color: #555;">Apertura Manual</h4>
      <input type="password" id="pinInput" class="pin-input" placeholder="PIN" maxlength="4">
      <br>
      <button id="btnManual" class="btn btn-blue" style="width: 80%;" onclick="toggleManual()">Abrir Permanente 🔓</button>
    </div>

  </div>

  <script>
    var gateway = `ws://${window.location.hostname}/ws`;
    var websocket;
    var puertaAbiertaManual = false; 

    window.addEventListener('load', onLoad);

    function onLoad(event) { initWebSocket(); }
    function initWebSocket() {
      websocket = new WebSocket(gateway);
      websocket.onopen = function(event) { document.getElementById('ws-status').innerText = "Conectado"; };
      websocket.onclose = function(event) { document.getElementById('ws-status').innerText = "Desconectado"; setTimeout(initWebSocket, 2000); };
      websocket.onmessage = function(event) {
        if(event.data.startsWith("ALERTA:")) {
          document.getElementById("uid-text").innerText = event.data.split(":")[1];
          document.getElementById("foto-intruso").src = "/foto?rand=" + Math.random(); 
          document.getElementById("alerta-container").style.display = "block";
          document.getElementById("step1").style.display = "block";
          document.getElementById("step2").style.display = "none";
        }
      };
    }

    function enviarComando(cmd) {
      websocket.send(cmd);
      if(cmd === 'PERMITIR' || cmd === 'DENEGAR') {
        document.getElementById("step1").style.display = "none";
        document.getElementById("step2").style.display = "block";
      } else if (cmd === 'CONSERVAR' || cmd === 'BORRAR') {
        document.getElementById("alerta-container").style.display = "none";
        alert(cmd === 'CONSERVAR' ? "Evidencia guardada en la MicroSD." : "Evidencia borrada de la MicroSD.");
      }
    }

    // LÓGICA DEL BOTÓN DE APERTURA MANUAL CON PIN 0507
    function toggleManual() {
      if (!puertaAbiertaManual) {
        let pin = document.getElementById("pinInput").value;
        if (pin === "0507") {
          websocket.send("MANTENER_ABIERTA");
          document.getElementById("btnManual").innerText = "Cerrar Puerta 🔒";
          document.getElementById("btnManual").className = "btn btn-gray";
          document.getElementById("pinInput").style.display = "none"; 
          puertaAbiertaManual = true;
        } else {
          alert("PIN incorrecto. Acceso denegado.");
          document.getElementById("pinInput").value = "";
        }
      } else {
        websocket.send("CERRAR_PUERTA");
        document.getElementById("btnManual").innerText = "Abrir Permanente 🔓";
        document.getElementById("btnManual").className = "btn btn-blue";
        document.getElementById("pinInput").style.display = "inline-block"; 
        document.getElementById("pinInput").value = "";
        puertaAbiertaManual = false;
      }
    }
  </script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- INICIALIZANDO CAMARA Y SERVO ---");
  
  // 1. Inicialización del hardware de captura
  inicializarCamara();
  
  // 2. Configuración inicial del Servo
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  cerradura.setPeriodHertz(50);
  cerradura.attach(SERVO_PIN, 500, 2400); 
  cerradura.write(0); // 0 grados = CERRADO

  // 3. Montaje del sistema de archivos
  inicializarSD();

  // Gestión de Credenciales de Red Local
  preferencias.begin("wifi_creds", false);
  ssid_actual = preferencias.getString("ssid", "");
  password_actual = preferencias.getString("password", "");

  Serial.println("Tienes 5 segundos para configurar el WiFi por Serial (Presiona 'C')...");
  long startTime = millis();
  bool cambiarWiFi = false;
  
  while (millis() - startTime < 5000) {
    if (Serial.available()) {
      if (toupper(Serial.read()) == 'C') { cambiarWiFi = true; break; }
    }
  }

  if (cambiarWiFi) {
    Serial.println("\n--- MODO CONFIGURACION ---");
    Serial.print("Nuevo SSID: "); while (!Serial.available()) delay(10); ssid_actual = Serial.readStringUntil('\n'); ssid_actual.trim();
    Serial.print("Nuevo Password: "); while (!Serial.available()) delay(10); password_actual = Serial.readStringUntil('\n'); password_actual.trim();
    preferencias.putString("ssid", ssid_actual); preferencias.putString("password", password_actual);
  }

  // --- LÓGICA HÍBRIDA DE CONEXIÓN WI-FI (AP FALLBACK) ---
  bool redConectada = false;

  if (ssid_actual != "") {
    Serial.println("Intentando conectar a red guardada...");
    redConectada = conectarWiFi(ssid_actual.c_str(), password_actual.c_str());
  }

  if (!redConectada) {
    Serial.println("Intentando credenciales por defecto...");
    redConectada = conectarWiFi(ssid_default, password_default);
  }

  if (redConectada) {
    WiFi.mode(WIFI_STA); 
    Serial.println("Modo Access Point deshabilitado para ahorrar memoria RAM.");
  } else {
    Serial.println("\n--- FALLO DE RED: INICIANDO ACCESS POINT (AP) ---");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Timbre_Inteligente_IoT", "12345678");
    Serial.print("Red de Respaldo Creada. IP del Dashboard: "); 
    Serial.println(WiFi.softAPIP());
  }

  // INTERCAMBIO DE ARQUITECTURA: LIBERACIÓN DE BUS SERIAL RX
  Serial.flush();
  Serial.begin(115200, SERIAL_8N1, 33, 1);
  
  Serial.println("\n--- INICIALIZANDO NFC Y TIMBRE ---");
  Wire.begin(SDA_PIN, SCL_PIN);
  inicializarNFC();
  
  pinMode(TIMBRE_PIN, INPUT_PULLUP);

  // Endpoints del Servidor Asíncrono
  ws.onEvent(onEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", index_html);
  });

  server.on("/foto", HTTP_GET, [](AsyncWebServerRequest *request){
    if(SD_MMC.exists("/temp_foto.jpg")) {
      request->send(SD_MMC, "/temp_foto.jpg", "image/jpeg");
    } else {
      request->send(404, "text/plain", "Foto no encontrada");
    }
  });

  server.begin();
  Serial.println("\n--- SISTEMA LISTO - ESTADO: IDLE ---");
}

void loop() {
  ws.cleanupClients(); 
  unsigned long tiempoActual = millis();

  switch (estadoActual) {
    case ESTADO_IDLE: { 
      if (digitalRead(TIMBRE_PIN) == LOW) {
        delay(50); 
        if (digitalRead(TIMBRE_PIN) == LOW) {
          Serial.println("¡Timbre Presionado!");
          ultimoUID = "TIMBRE MANUAL";
          estadoActual = ESTADO_RUN_ALERTA;
        }
      }
      
      uint8_t success;
      uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
      uint8_t uidLength;
      
      success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50);
      if (success) {
        bool match = true;
        ultimoUID = "";
        for (uint8_t i = 0; i < 4; i++) {
          ultimoUID += String(uid[i], HEX);
          if (uid[i] != uidAutorizado[i]) match = false;
        }
        
        if (match) {
          Serial.println("Acceso Autorizado por NFC.");
          cerradura.attach(SERVO_PIN, 500, 2400); 
          estadoActual = ESTADO_RUN_ACCESO;
          timerEstado = millis(); 
        } else {
          Serial.println("¡Intruso Detectado por NFC!");
          estadoActual = ESTADO_RUN_ALERTA;
        }
        delay(1000); 
      }
      break;
    } 

    case ESTADO_RUN_ACCESO:
      cerradura.write(90); // 90 = ABIERTO
      if (tiempoActual - timerEstado >= 3000) {
        cerradura.write(0); // 0 = CERRADO
        estadoActual = ESTADO_IDLE;
        Serial.println("Puerta cerrada automáticamente. Retornando a IDLE.");
      }
      break;

    case ESTADO_RUN_ALERTA:
      capturarYGuardarFoto();
      ws.textAll("ALERTA:" + ultimoUID); 
      estadoActual = ESTADO_WAIT;
      break;

    case ESTADO_WAIT:
      // El sistema está bloqueado esperando la decisión de PERMITIR o DENEGAR (y su respectiva foto)
      break;
      
    case ESTADO_MANTENER_ABIERTA:
      // Espera indefinidamente la orden CERRAR_PUERTA
      break;
  }
}

// Procesador de Mensajes WebSocket en Tiempo Real
void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_DATA) {
    String comando = "";
    for(size_t i=0; i < len; i++) comando += (char) data[i];
    
    Serial.println("Comando WebSocket recibido: " + comando);

    if(comando == "PERMITIR") {
      cerradura.attach(SERVO_PIN, 500, 2400); 
      estadoActual = ESTADO_RUN_ACCESO;
      timerEstado = millis(); // Dispara el contador de 3 segundos
    } 
    else if (comando == "DENEGAR") {
      // No cambiamos el estado, se queda en ESTADO_WAIT esperando la decisión de la foto
      Serial.println("Acceso Denegado. Esperando accion sobre la evidencia...");
    } 
    else if (comando == "BORRAR") {
      if(SD_MMC.exists("/temp_foto.jpg")) SD_MMC.remove("/temp_foto.jpg");
      Serial.println("Foto borrada de la SD.");
      
      // Solo volvemos a IDLE si venimos de un DENEGAR (que nos dejó en WAIT)
      if (estadoActual == ESTADO_WAIT) {
        estadoActual = ESTADO_IDLE;
        Serial.println("Evidencia borrada. Sistema liberado a IDLE.");
      }
      // Si estadoActual es RUN_ACCESO (porque se pulsó Permitir), el timer de 3s seguirá su curso
    } 
    else if (comando == "CONSERVAR") {
      Serial.println("Foto conservada en la SD.");
      
      // Solo volvemos a IDLE si venimos de un DENEGAR
      if (estadoActual == ESTADO_WAIT) {
        estadoActual = ESTADO_IDLE;
        Serial.println("Evidencia conservada. Sistema liberado a IDLE.");
      }
    }
    else if (comando == "MANTENER_ABIERTA") {
      cerradura.attach(SERVO_PIN, 500, 2400); 
      cerradura.write(90); 
      estadoActual = ESTADO_MANTENER_ABIERTA;
      Serial.println("Modo Manual: Puerta mantenida ABIERTA permanentemente.");
    } 
    else if (comando == "CERRAR_PUERTA") {
      cerradura.attach(SERVO_PIN, 500, 2400); 
      cerradura.write(0); 
      estadoActual = ESTADO_IDLE;
      Serial.println("Modo Manual: Puerta CERRADA. Retornando a IDLE.");
    }
  }
}

// Funciones de Gestión de Hardware
void inicializarNFC() {
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("No se encontro el modulo PN532. Recuerda quitar el cable TX del FTDI.");
  } else {
    Serial.println("Modulo PN532 inicializado correctamente.");
    nfc.SAMConfig(); 
  }
}

void capturarYGuardarFoto() {
  if (!camaraLista) {
    Serial.println("Fotografia cancelada: La camara no esta lista.");
    return;
  }

  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) { Serial.println("Error al tomar foto"); return; }
  
  if(SD_MMC.exists("/temp_foto.jpg")) SD_MMC.remove("/temp_foto.jpg");
  File file = SD_MMC.open("/temp_foto.jpg", FILE_WRITE);
  if(!file){
    Serial.println("Error abriendo archivo en MicroSD");
  } else {
    file.write(fb->buf, fb->len);
    file.close();
    Serial.println("Foto guardada en MicroSD como temp_foto.jpg");
  }
  esp_camera_fb_return(fb); 
}

void inicializarCamara() {
  if (!psramFound()) {
    Serial.println("¡ERROR: PSRAM no detectada! Verifica la configuracion.");
    camaraLista = false;
    return;
  }
  
  camera_config_t config;
  
  config.ledc_channel = LEDC_CHANNEL_7; 
  config.ledc_timer = LEDC_TIMER_3;     
  
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA; 
  config.jpeg_quality = 12;
  config.fb_count = 1;
  
  config.fb_location = CAMERA_FB_IN_PSRAM; 
  config.grab_mode = CAMERA_GRAB_LATEST;   
  
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) { 
    Serial.printf("Error camara: 0x%x\n", err); 
    camaraLista = false;
    return; 
  }
  
  camaraLista = true;
  Serial.println("Camara inicializada correctamente (PSRAM OK).");
}

void inicializarSD() {
  if(!SD_MMC.begin("/sdcard", true)){ Serial.println("Fallo montaje SD"); return; }
  Serial.println("MicroSD iniciada en modo 1-bit.");
}

bool conectarWiFi(const char* ssid, const char* pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  int intentos = 0;
  Serial.print("Conectando a WiFi: ");
  Serial.println(ssid);
  
  while (WiFi.status() != WL_CONNECTED && intentos < 20) { 
    delay(500); 
    Serial.print("."); 
    intentos++; 
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nWiFi Conectado IP: "); Serial.println(WiFi.localIP());
    return true;
  }
  
  Serial.println("\nFallo la conexion al WiFi.");
  return false;
}
