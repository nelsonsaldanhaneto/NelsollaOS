#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <OneButton.h>

#include "structs.h"
#include "images.h"
#include "ConfigManager.h"
#include "TimeService.h"
#include "CalendarService.h"
#include "WeatherService.h"
#include "AirQualityService.h"
#include "DaylightService.h"
#include "DisplayService.h"
#include "WebServerService.h"
#include "CryptoService.h"
#include "CurrencyService.h"
#include "StockService.h"
#include "PcMonitorService.h"
#include "BambuService.h"
#include "CellairisService.h"
#include "GameService.h"

// Global Constants
const char* AP_SSID = "NelsollaOS";
const char* AP_PASS = "NelsollaOS";
// Deliberately still upstream's namespace: renaming it points Preferences at a
// fresh NVS area, orphaning every saved setting (location, screen order, theme)
// on the first boot after flashing.
const char* PREF_NAMESPACE = "tinytosh_config";

// Timing & Night Mode Constants
const unsigned long NORMAL_REFRESH_MS = 1000;
const unsigned long NIGHT_DIM_REFRESH_MS = 10000;
const unsigned long NIGHT_DISPLAY_OFF_REFRESH_MS = 60000;
const unsigned long NIGHT_WAKE_DURATION_MS = 30000;
const int NIGHT_DATA_INTERVAL_MULTIPLIER = 10;

const int CONTRAST_DIM = 1;
const int CONTRAST_MAX = 255;

// Hardware Pins
const int DEFAULT_TOUCH_PIN = 10;
const int DEFAULT_SDA_PIN = 8;
const int DEFAULT_SCL_PIN = 9;
const int MIN_GPIO_PIN = 0;
const int MAX_GPIO_PIN = 21;

// Global Data Structure
AppState appState;

// Forward declaration of callback for WebServerService
void updateAllDataCallback();

// Service Instances
ConfigManager configManager(PREF_NAMESPACE);
DisplayService displayService(128, 64, -1);
WebServerService webServerService(80, updateAllDataCallback);
TimeService timeService;
CalendarService calendarService;
WeatherService weatherService;
AirQualityService airQualityService;
DaylightService daylightService;
CryptoService cryptoService;
CurrencyService currencyService;
StockService stockService;
PcMonitorService pcMonitorService;
BambuService bambuService;
CellairisService cellairisService;
GameService gameService;

unsigned long lastScreenSwitch = 0;
int currentScreen = 0;
int currentSubScreen = 0;
bool nightModeLatched = false;

OneButton button;
unsigned long lastInteractionTime = 0; 
unsigned long lastScreenUpdate = 0;

// Background Task & Sync Trackers
bool isBackgroundUpdating = false;
TaskHandle_t bgUpdateTaskHandle = NULL;
bool pendingDataUpdate = false;

// Helper Functions

void drawCurrentScreen() {
  displayService.drawScreen(currentScreen, appState, currentSubScreen);
}

// animate=false troca instantaneamente. As animacoes sao loops bloqueantes de
// ~300-600ms (frames I2C + delay) em que button.tick() nao roda, entao toques
// rapidos seguidos eram engolidos — justamente o caso do easter egg.
void switchToNextScreen(bool animate = true) {
  if (currentScreen == SCREEN_STOCK && currentSubScreen + 1 < appState.config.stock_count) {
      currentSubScreen++;
      if (animate) displayService.animateTransition(currentScreen, currentSubScreen - 1, currentScreen, currentSubScreen, appState);
      return;
  }
  if (currentScreen == SCREEN_CRYPTO && currentSubScreen + 1 < appState.config.crypto_count) {
      currentSubScreen++;
      if (animate) displayService.animateTransition(currentScreen, currentSubScreen - 1, currentScreen, currentSubScreen, appState);
      return;
  }
  if (currentScreen == SCREEN_CURRENCY && currentSubScreen + 1 < appState.config.currency_count) {
      currentSubScreen++;
      if (animate) displayService.animateTransition(currentScreen, currentSubScreen - 1, currentScreen, currentSubScreen, appState);
      return;
  }

  int oldScreen = currentScreen;
  int oldSubScreen = currentSubScreen;
  currentSubScreen = 0; 

  int currentIndex = 0;
  for (int i = 0; i < NUM_SCREENS; i++) {
    if (appState.config.screen_order[i] == currentScreen) {
      currentIndex = i; break;
    }
  }

  int checkIndex = currentIndex;
  int nextScreenCandidate = currentScreen;
  do {
    checkIndex++;
    if (checkIndex >= NUM_SCREENS) checkIndex = 0;
    int candidateId = appState.config.screen_order[checkIndex];
    if (displayService.isScreenEnabled(appState, candidateId)) {
      nextScreenCandidate = candidateId;
      break;
    }
  } while (checkIndex != currentIndex);

  if (oldScreen == nextScreenCandidate && oldSubScreen == 0) return;

  if (animate) displayService.animateTransition(oldScreen, oldSubScreen, nextScreenCandidate, 0, appState);
  currentScreen = nextScreenCandidate;
}

int getFirstEnabledScreen() {
  for (int i = 0; i < NUM_SCREENS; i++) {
    int screenId = appState.config.screen_order[i];
    if (displayService.isScreenEnabled(appState, screenId)) {
      return screenId;
    }
  }
  return appState.config.screen_order[0];
}

bool isTimeInWindow(int currentMins, String startStr, String endStr) {
  int startH = startStr.substring(0, 2).toInt();
  int startM = startStr.substring(3, 5).toInt();
  int startMins = startH * 60 + startM;

  int endH = endStr.substring(0, 2).toInt();
  int endM = endStr.substring(3, 5).toInt();
  int endMins = endH * 60 + endM;

  if (startMins < endMins) return (currentMins >= startMins && currentMins < endMins);
  else return (currentMins >= startMins || currentMins < endMins);
}

int getActiveNightAction() {
  if (!appState.config.night_mode) return -1;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return -1;
  int currentMins = timeinfo.tm_hour * 60 + timeinfo.tm_min;

  if (appState.config.night_action == 3) {
    if (isTimeInWindow(currentMins, appState.config.night_start, appState.config.night_end)) return 2;
    if (isTimeInWindow(currentMins, appState.config.night_dim_start, appState.config.night_end)) return 1;
    return -1;
  }

  if (isTimeInWindow(currentMins, appState.config.night_start, appState.config.night_end)) {
    return appState.config.night_action;
  }
  
  return -1;
}

void validatePins(Config& config) {
  bool used_pins[MAX_GPIO_PIN + 1] = {false};

  auto claimPin = [&](int &pin, int default_pin) {
    
    // 1. Sanitize bounds
    if (pin < MIN_GPIO_PIN || pin > MAX_GPIO_PIN) {
      pin = default_pin;
    }

    // 2. Check for collision
    if (used_pins[pin]) {
      pin = default_pin;
      
      // 3. Find the absolute first available free pin
      if (used_pins[pin]) {
        for (int i = MIN_GPIO_PIN; i <= MAX_GPIO_PIN; i++) {
          if (!used_pins[i]) {
            pin = i;
            break;
          }
        }
      }
    }
    
    // 4. Mark the final chosen pin as claimed
    used_pins[pin] = true;
  };

  claimPin(config.sda_pin, DEFAULT_SDA_PIN);
  claimPin(config.scl_pin, DEFAULT_SCL_PIN);
  claimPin(config.touch_pin, DEFAULT_TOUCH_PIN);
}

void configureHardware() {
  validatePins(appState.config);
  
  button.setup(appState.config.touch_pin, INPUT, false);
  button.attachClick(handleSingleClick);
  button.attachLongPressStart(handleLongPressStart);
  button.attachDuringLongPress(handleDuringLongPress);
  button.attachLongPressStop(handleLongPressStop);
  button.attachMultiClick(handleMultiClick);
  // O TTP223 ja debounce em hardware; 50ms aqui engolia toques rapidos.
  button.setDebounceTicks(25); 
  button.setClickTicks(100);
  button.setPressTicks(750);
  button.setLongPressIntervalMs(60);   // cadencia da barra de progresso do gesto
  button.reset();
  
  Serial.printf("Hardware Configured: SDA=%d, SCL=%d, TOUCH=%d\n", appState.config.sda_pin, appState.config.scl_pin, appState.config.touch_pin);
}

// Core Application Logic

// Easter egg: NELSOLLA RUN abre com um clique seguido de segurar 3s.
//
// O discriminador vem da propria maquina de estados do OneButton: ao voltar de
// OCS_COUNT para OCS_DOWN ela NAO zera _nClicks, entao no longPressStart
// getNumberClicks() vale 0 num hold puro (= travar rotacao, comportamento de
// sempre) e >=1 quando um clique precedeu o hold (= gesto do jogo).
const unsigned long GAME_HOLD_MS = 3000;
bool gameGestureArmed = false;

void doClickAction() {
  // Toque logo apos o anterior = usuario tamborilando (provavelmente atras do
  // easter egg). Pula a animacao pra nao bloquear a leitura dos proximos.
  static unsigned long lastClickAt = 0;
  bool rapid = (millis() - lastClickAt) < 600;
  lastClickAt = millis();

  int activeAction = getActiveNightAction();
  bool wasScreenOff = (nightModeLatched && activeAction == 2 && (millis() - lastInteractionTime >= NIGHT_WAKE_DURATION_MS));

  if (wasScreenOff) {
    Serial.println("🌙 Night Mode: Waking display temporarily on Primary Screen.");
    currentScreen = getFirstEnabledScreen();
    
    displayService.display.ssd1306_command(SSD1306_SETCONTRAST);
    displayService.display.ssd1306_command(CONTRAST_DIM);
  } else {
    Serial.println("👆 Button Pressed: Switching Screen");
    if (nightModeLatched) {
      displayService.display.ssd1306_command(SSD1306_SETCONTRAST);
      displayService.display.ssd1306_command((activeAction == 0) ? CONTRAST_MAX : CONTRAST_DIM);
    }
    switchToNextScreen(!rapid);
  }
  
  lastScreenSwitch = millis();
  lastInteractionTime = millis();
  lastScreenUpdate = 0;
}

void handleSingleClick() {
  doClickAction();
}

void handleMultiClick() {
  // 2+ toques rapidos: mantem o avanco de tela, senao tamborilar nao faria nada.
  doClickAction();
}

void handleLongPressStart() {
  if (button.getNumberClicks() >= 1) {
    gameGestureArmed = true;
    Serial.println("GameService: gesto do jogo armado (clique + segurar)...");
    displayService.drawGameHoldProgress(0);
    return;
  }
  toggleAutoCycle();
}

void handleDuringLongPress() {
  if (!gameGestureArmed) return;

  unsigned long held = button.getPressedMs();
  if (held >= GAME_HOLD_MS) {
    gameGestureArmed = false;
    gameService.start();
    return;
  }
  displayService.drawGameHoldProgress((int)((held * 100) / GAME_HOLD_MS));
}

void handleLongPressStop() {
  if (!gameGestureArmed) return;
  // Soltou antes dos 3s: aborta sem efeito e devolve a tela normal.
  gameGestureArmed = false;
  Serial.println("GameService: gesto abortado.");
  lastScreenUpdate = 0;
}

void toggleAutoCycle() {
  appState.config.screen_auto_cycle = !appState.config.screen_auto_cycle;

  if (appState.config.screen_auto_cycle) {
    Serial.println("🔄 Auto Cycle: ENABLED");
    displayService.drawInfoScreen(icon_unlock, "Rotacao Ligada");
    displayService.display.display();
  } else {
    Serial.println("🔒 Auto Cycle: DISABLED (Screen Locked)");
    displayService.drawInfoScreen(icon_lock, "Rotacao Travada");
    displayService.display.display();
  }
  
  configManager.saveConfig(appState.config);
  
  delay(1000); 
  
  lastScreenUpdate = 0; 
  lastScreenSwitch = millis(); 
  lastInteractionTime = millis();
}

void updateAllData() {
  nightModeLatched = false;

  // 1. Location Detection
  if (appState.config.auto_detect) {
    displayService.showOLEDStatus({"\n", "\n", "Detectando Local...", "\n", "Aguarde..."}, true);
    if (timeService.fetchLocationData(appState.config)) {
      Serial.println("Location updated via IP");
    }
  }

  // 2. Sync Time (Depends on Location/Timezone)
  displayService.showOLEDStatus({"\n", "\n", "Sincronizando Hora...", "\n", "Fuso:", appState.config.timezone}, true);
  timeService.syncNTP(appState.config.timezone);

  struct tm timeinfo;
  getLocalTime(&timeinfo);
  int current_year = timeinfo.tm_year + 1900;
  int current_yday = timeinfo.tm_yday;

  // 3. Fetch Calendar (Holidays - Depends on Country (Country Code))
  if (appState.config.show_calendar && appState.config.calendar_show_holidays && appState.calendar.last_fetch_year != current_year) {  
    displayService.showOLEDStatus({"\n", "\n", "Atualizando Calendario...", "\n", "Pais:", appState.config.country}, true);
    calendarService.fetchHolidays(appState.config.country_code, appState.calendar);
  }

  // 4. Fetch Weather (Depends on Lat/Lon)
  if (appState.config.show_weather) {  
    displayService.showOLEDStatus({"\n", "\n", "Atualizando Clima...", "\n", "Local:", appState.config.city}, true);
    String updateTime = TimeService::getCurrentTime(appState.config.time_format);
    weatherService.fetchWeather(appState.config, appState.weather, updateTime);
  }

  // 5. Fetch Air Quality (Depends on Lat/Lon)
  if (appState.config.show_aqi) {  
    displayService.showOLEDStatus({"\n", "\n", "Atualizando Qualid. Ar...", "\n", "Local:", appState.config.city}, true);
    airQualityService.fetchAirQuality(appState.config, appState.aqi);
  }

  // 6. Fetch Daylight (Depends on Lat/Lon)
  if (appState.config.show_daylight && appState.daylight.last_fetch_yday != current_yday) {  
    displayService.showOLEDStatus({"\n", "\n", "Atualizando Sol...", "\n", "Local:", appState.config.city}, true);
    daylightService.fetchDaylight(appState.config, appState.daylight);
  }

  // 7. Fetch Stocks (Independent)
  if (appState.config.show_stock) {
    for (int i = 0; i < appState.config.stock_count; i++) {
      displayService.showOLEDStatus({"\n", "\n", "Atualizando Acoes...", "\n", "Acao:", appState.config.stock_symbols[i]}, true);
      stockService.fetchStock(appState.config.stock_symbols[i], appState.stocks[i]);
    }
  }

  // 8. Fetch Crypto (Independent)
  if (appState.config.show_crypto) { 
    for (int i = 0; i < appState.config.crypto_count; i++) {
      displayService.showOLEDStatus({"\n", "\n", "Atualizando Cripto...", "\n", "Ticker ID:", String(appState.config.crypto_ids[i])}, true);
      cryptoService.fetchPrice(appState.config.crypto_ids[i], appState.cryptos[i]);
    }
  }

  // 9. Fetch Currency (Independent)
  if (appState.config.show_currency) { 
    for (int i = 0; i < appState.config.currency_count; i++) {
      String baseUpper = String(appState.config.currency_bases[i]);
      baseUpper.toUpperCase();
      String targetUpper = String(appState.config.currency_targets[i]);
      targetUpper.toUpperCase();

      displayService.showOLEDStatus({"\n", "\n", "Atualizando Moedas...", "\n", "Par:", baseUpper + " -> " + targetUpper}, true);
      currencyService.fetchRate(appState.config.currency_bases[i], appState.config.currency_targets[i], appState.currencies[i]);
    }
  }

  // 10. Fetch Cellairis (Independent)
  if (appState.config.show_cellairis) {
    displayService.showOLEDStatus({"\n", "\n", "Atualizando Cellairis...", "\n", "Faturamento do mes"}, true);
    cellairisService.fetchSummary(appState.config, appState.cellairis);
  }

  displayService.showOLEDStatus({"\n", "\n", "Dados Atualizados", "\n", "\n", "NelsollaOS Pronto", "\n", "\n", "Bem-vindo!"}, true);

  // 10. Save Everything
  configManager.saveConfig(appState.config);

  // 11. Find the first enabled screen to show immediately
  currentScreen = getFirstEnabledScreen();
  lastScreenSwitch = millis();
}

// Global function wrapper for the class method
void updateAllDataCallback() {
  updateAllData();
}

// Background Task for Scheduled Updates
void backgroundDataFetchTask(void* parameter) {
  Serial.println("Background Task: Starting API updates...");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Background Task: WiFi offline. Aborting updates.");
    isBackgroundUpdating = false;
    bgUpdateTaskHandle = NULL;
    vTaskDelete(NULL);
    return;
  }
  
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  int current_year = timeinfo.tm_year + 1900;
  int current_yday = timeinfo.tm_yday;

  if (appState.config.show_weather) weatherService.fetchWeather(appState.config, appState.weather, TimeService::getCurrentTime(appState.config.time_format));
  if (appState.config.show_aqi) airQualityService.fetchAirQuality(appState.config, appState.aqi);
  if (appState.config.show_daylight && appState.daylight.last_fetch_yday != current_yday) daylightService.fetchDaylight(appState.config, appState.daylight);
  if (appState.config.show_crypto) {
    for (int i = 0; i < appState.config.crypto_count; i++) cryptoService.fetchPrice(appState.config.crypto_ids[i], appState.cryptos[i]);
  }
  if (appState.config.show_currency) {
    for (int i = 0; i < appState.config.currency_count; i++) currencyService.fetchRate(appState.config.currency_bases[i], appState.config.currency_targets[i], appState.currencies[i]);
  }
  if (appState.config.show_stock) {
    for (int i = 0; i < appState.config.stock_count; i++) stockService.fetchStock(appState.config.stock_symbols[i], appState.stocks[i]);
  }
  if (appState.config.show_calendar && appState.config.calendar_show_holidays && appState.calendar.last_fetch_year != current_year) calendarService.fetchHolidays(appState.config.country_code, appState.calendar);
  if (appState.config.show_cellairis) cellairisService.fetchSummary(appState.config, appState.cellairis);

  Serial.println("Background Task: Updates complete.");
  isBackgroundUpdating = false;
  bgUpdateTaskHandle = NULL;
  vTaskDelete(NULL);
}

void setup() {
  Serial.setRxBufferSize(1024);
  Serial.begin(115200);
  delay(100);

  configManager.loadConfig(appState.config); 
  configureHardware();

  displayService.begin(appState.config.sda_pin, appState.config.scl_pin);
  delay(3000);

  displayService.showOLEDStatus({"\n", "\n", "Iniciando...", "\n", "\n", "Config Carregada!"}, true);
  bambuService.begin(&appState.config, &appState.bambu);
  gameService.begin(&displayService.display, appState.config.touch_pin);

  WiFiManager wm;
  wm.setConnectTimeout(15);
  wm.setConnectRetries(3);

  wm.setAPCallback([](WiFiManager* m) {
    displayService.showOLEDStatus({"\n", "WiFi desconectado", "\n", "Conecte na rede:", AP_SSID, "\n", "Senha:", AP_PASS}, true);
  });

  displayService.showOLEDStatus({"\n", "\n", "Conectando...", "\n", "\n", "Buscando WiFi..."}, true);

  if (wm.autoConnect(AP_SSID, AP_PASS)) {
    String ipAddress = WiFi.localIP().toString();
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String uniqueName = "nelsollaos-" + mac.substring(8);
    uniqueName.toLowerCase();

    Serial.println("WiFi Connected!"); 
    Serial.print("IP Address: "); 
    Serial.println(ipAddress);

    appState.config.device_id = uniqueName;
    appState.config.ip_address = ipAddress;
    displayService.showOLEDStatus({
        "Connected to WiFi!", 
        "", 
        "IP: " + ipAddress, 
        "Name: " + uniqueName, 
        "", 
        "Loading..."
    }, true);

    delay(3000); 

    // 4. Initial Data Fetch (Synchronous)
    updateAllData();

  } else {
    Serial.println("Failed to connect and timed out. Staying in AP Mode.");
    displayService.showOLEDStatus({"\n", "Falha na conexao!", "\n", "Configure o WiFi no painel."}, true);
  }

  // 5. Initialize Web Server
  webServerService.setAppState(&appState);
  webServerService.begin();
}

void loop() {
  webServerService.handleClient();

  // Modo jogo: o GameService le o botao direto (sem debounce do OneButton,
  // que atrasaria o pulo). Ao sair, reseta o OneButton pra nao disparar um
  // clique fantasma com o estado que ficou pela metade.
  static bool wasGameActive = false;
  if (gameService.isActive()) {
    wasGameActive = true;
    gameService.update();
    return;
  }
  if (wasGameActive) {
    wasGameActive = false;
    button.reset();
    lastScreenUpdate = 0;   // forca redesenho da tela normal
    lastScreenSwitch = millis();
  }

  bambuService.loop();
  button.tick();

  if (pcMonitorService.handleSerial(appState)) {
    Serial.println("Config updated via USB! Saving and applying...");
    configManager.saveConfig(appState.config);
    updateAllData();
  }

  // 1. Night Latch Logic
  int activeAction = getActiveNightAction();
  bool nightScheduleActive = (activeAction != -1);

  if (!nightScheduleActive) {
    if (nightModeLatched) {
      Serial.println("☀️ Morning reached: Exiting Night Mode and resuming normal operation.");
      nightModeLatched = false;
      
      // If we are exiting mode 2 or 3 (display was off), reset to the main screen
      if (appState.config.night_action >= 2) {
        currentScreen = getFirstEnabledScreen();
      }
      
      lastScreenUpdate = 0;        
      lastScreenSwitch = millis();
    }
  } else if (!nightModeLatched) {
    if (currentScreen == getFirstEnabledScreen()) {
      Serial.println("🌙 Night Mode Latched: Reached the primary screen. Applying night settings.");
      nightModeLatched = true;
      lastScreenUpdate = 0; 
      lastInteractionTime = millis() - NIGHT_WAKE_DURATION_MS;
    }
  }

  // 2. Scheduled Data Refresh (Non-Blocking via FreeRTOS Task)
  static unsigned long lastDataUpdate = 0;
  unsigned long dataInterval = appState.config.refresh_interval_min * 60 * 1000;
  
  if (nightModeLatched) {
    dataInterval *= NIGHT_DATA_INTERVAL_MULTIPLIER;
  }

  if (millis() - lastDataUpdate > dataInterval) {
    if (!isBackgroundUpdating) {
        isBackgroundUpdating = true;
        Serial.println("Spawning background data update task...");
        xTaskCreate(
            backgroundDataFetchTask, 
            "BgUpdateTask", 
            8192,
            NULL, 
            1,
            &bgUpdateTaskHandle
        );
    }
    lastDataUpdate = millis();
  }

  // 3. Auto Screen Switching Logic
  if (appState.config.screen_auto_cycle && !nightModeLatched) {
    unsigned long intervalMs = appState.config.screen_interval_sec * 1000;

    if (millis() - lastScreenSwitch >= intervalMs) {
      switchToNextScreen();
      lastScreenSwitch = millis();
    }
  }

  // 4. Screen Redraw & Visual Action Logic
  static bool screenClearedForNight = false;

  bool isScreenOffAction = (nightModeLatched && activeAction == 2);
  bool isTemporarilyAwake = isScreenOffAction && (millis() - lastInteractionTime < NIGHT_WAKE_DURATION_MS);

  bool shouldDrawScreen = !isScreenOffAction || isTemporarilyAwake;

  if (!shouldDrawScreen) {
    if (!screenClearedForNight) {
      displayService.display.clearDisplay();
      displayService.display.display();
      screenClearedForNight = true;
      Serial.println("💤 Night Mode: Display turned OFF to save power. Waiting for interaction or morning.");
    }
  } else {
    if (screenClearedForNight) {
      screenClearedForNight = false;
      Serial.println("💡 Night Mode: Display turned back ON.");
    }
    
    unsigned long refreshInterval = NORMAL_REFRESH_MS;

    if (nightModeLatched) {
      refreshInterval = (activeAction == 2) ? NIGHT_DISPLAY_OFF_REFRESH_MS : NIGHT_DIM_REFRESH_MS;
    }

    if (millis() - lastScreenUpdate >= refreshInterval || lastScreenUpdate == 0) { 
        
      if (nightModeLatched) {
        if (activeAction == 1 || isTemporarilyAwake) {
          displayService.display.ssd1306_command(SSD1306_SETCONTRAST);
          displayService.display.ssd1306_command(CONTRAST_DIM); 
        } else if (activeAction == 0) {
          displayService.display.ssd1306_command(SSD1306_SETCONTRAST);
          displayService.display.ssd1306_command(CONTRAST_MAX);
        }
      } else {
        displayService.display.ssd1306_command(SSD1306_SETCONTRAST);
        displayService.display.ssd1306_command(CONTRAST_MAX);
      }

      drawCurrentScreen();
      displayService.display.display();
      lastScreenUpdate = millis();
    }
  }
}