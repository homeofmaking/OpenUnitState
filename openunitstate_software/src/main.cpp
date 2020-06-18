#include <Arduino.h> 
#include <SPI.h>
#include <MFRC522.h>
#include <Ticker.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <ArduinoOTA.h>
#include "secret.h"

#define FW_VERSION "0.24"

// Chip ID
uint32_t chipid = ESP.getChipId();
char clientid[12];

// RFID
MFRC522 rfid(PIN_RFID_CHIP_SELECT, PIN_RFID_RESET); // Instance of the class
MFRC522::MIFARE_Key key; 
byte nuidPICC[4];

// Pre-declaration 
void setupWiFi();
void setupMQTT();
void unitLock();
void unitUnlock();
void pushToUnlockRequest();
boolean idleDisplay();
char* ip2CharArray(IPAddress IP);
String printHex(byte *buffer, byte bufferSize);

// Various objects setup
WiFiManager wifiManager;
WiFiClient espClient;
PubSubClient mqttClient(espClient);
LiquidCrystal_I2C lcd(0x27,16,2);  


// Display
String displayLineOne = "    UNKNOWN        ";
String displayLineTwo = "     ERROR         ";
char spinnerSymbols[] = {'.', 'o', 'O', 'o'};
uint8_t spinnerTick = 0;
byte lockChar[8] = {0b01110,0b10001,0b10001,0b11111,0b11011,0b11011,0b11111,0b00000};
byte checkChar[8] = {0b00000,0b00001,0b00011,0b10110,0b11100,0b01000,0b00000,0b00000};
byte skullChar[8] = {0b00000,0b01110,0b10101,0b11011,0b01110,0b01110,0b00000,0b00000};
byte buttonChar[8] = {0b00000,0b00100,0b00100,0b01110,0b00100,0b00000,0b01110,0b11111};
bool displayBlinkingState = false;
bool keepPressingToReport = false;
int displayScrollTick = 0;
int displayLongReasonLength;

// Config
String unitName = "";
String messageFlashLineTwo = "";
uint32_t unlockedTime = 0;

boolean maintenanceMode = true;
String maintenanceLongReason = "";

enum class Mode {
  requiresAuth,
  pushToUnlock,
  permUnlocked,
  waitingForOTA,
  checkInStation
};
Mode mode;

// Button config
volatile long lastDebounceTime = 0;
const int debounceDelay = 2000;

// Timers
void readCard();
void displayUpdate();
void machinerelock();
void reportBroken();
void quickdisplayclear();
Ticker timer_readcard(readCard, 100);
Ticker timer_displayupdate(displayUpdate, 1000);  
Ticker timer_machinerelock(machinerelock, 0, 1, MILLIS);
Ticker timer_reportbroken(reportBroken, 3000, 1, MILLIS);
Ticker timer_quickdisplayclear(quickdisplayclear, 5100, 1, MILLIS);



void setupWiFi() {
  lcd.clear(); lcd.print("Connecting WiFi");

  char hostName[32];
  sprintf(hostName, "%s-%06X", HOSTNAME, ESP.getChipId());
  wifi_station_set_hostname(hostName);

  if (!wifiManager.autoConnect()) {
    Serial.println("failed to connect, we should reset as see if it connects");
    lcd.clear(); lcd.print("  WiFi ERROR!");
    delay(1000);
    ESP.restart();
  }
  Serial.print(String(ESP.getChipId()) + " connected to WiFi - IP Addr. ");
  Serial.println(WiFi.localIP());

  randomSeed(micros());
}

void setupMQTT() {
  if (!mqttClient.connected()) {
    if (WiFi.status() != WL_CONNECTED) {
       return;
    }
    
    lcd.clear(); lcd.print("Connecting MQTT");

    Serial.print("Attempting MQTT connection... ");
    String mqttclientId = HOSTNAME;
    mqttclientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (mqttClient.connect(mqttclientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      String topic = String(MQTT_TOPIC) + String(clientid) + "/connected";
      mqttClient.publish(topic.c_str(), "true");
      // ... and resubscribe
      String sub_topic = String(MQTT_TOPIC) + String(clientid) + "/+";
      mqttClient.subscribe(sub_topic.c_str());
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      lcd.clear(); lcd.print("  MQTT ERROR!");
      delay(5000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.println();
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  char message[length];
  memcpy(message, payload, length);
  // add NULL terminator to message, making it a correct c string
  message[length] = '\0';

  Serial.print(message);

  String topic_config = String(MQTT_TOPIC) + String(clientid) + "/config_name";
  String topic_mtnc_long_reason = String(MQTT_TOPIC) + String(clientid) + "/config_maintenance_long_reason";
  String topic_quick_display_msg = String(MQTT_TOPIC) + String(clientid) + "/quick_display_msg";
  String topic_status = String(MQTT_TOPIC) + String(clientid) + "/config_status";
  String topic_ultime = String(MQTT_TOPIC) + String(clientid) + "/unlocked_time";
  String topic_reset = String(MQTT_TOPIC) + String(clientid) + "/reset";

  if (strcmp(topic, topic_config.c_str())==0) {
    unitName = String(message);
    idleDisplay();
  } else if (strcmp(topic, topic_mtnc_long_reason.c_str())==0) {
    maintenanceLongReason = String(message);
    idleDisplay();
  } else if (strcmp(topic, topic_quick_display_msg.c_str())==0) {
    timer_quickdisplayclear.start();
    messageFlashLineTwo = String(message);
  } else if (strcmp(topic, topic_ultime.c_str())==0) {
    if(mode != Mode::permUnlocked) {
      unlockedTime = strtoul(String(message).c_str(), NULL, 10);
      timer_machinerelock.interval(unlockedTime);
      timer_machinerelock.start();
      unlockedTime = unlockedTime;
      unitUnlock();
    } else {
      Serial.print("Tried unlocked_time call but machine is perm unlocked...");
    }
  } else if (strcmp(topic, topic_reset.c_str())==0) {
    ESP.restart();
  } else if (strcmp(topic, topic_status.c_str())==0) {
    maintenanceMode = false;
    if ((char)payload[0] == '5') {
      // Auth required 
      mode = Mode::requiresAuth;
      if(!timer_machinerelock.state() == RUNNING) {
        unitLock();
      }
    } else if ((char)payload[0] == '2') {
      // Working & Push to unlock
      mode = Mode::pushToUnlock;
      if(!timer_machinerelock.state() == RUNNING) {
        unitLock();
      }
    } else if ((char)payload[0] == '0') {
      // Working & Unlocked permanently
      mode = Mode::permUnlocked;
      unitUnlock();
    } else if ((char)payload[0] == '-') {
      // Not working & special modes
      if ((char)payload[1] == '1') {
        // maintenanceMode
        maintenanceMode = true;
        unitLock();
      } else if((char)payload[1] == '2') {
        maintenanceMode = true;
        mode = Mode::waitingForOTA;
        unitLock();
        ArduinoOTA.begin();
        
        String topic = String(MQTT_TOPIC) + String(clientid) + "/ready_for_ota";
        mqttClient.publish(topic.c_str(), clientid);
      } else if((char)payload[1] == '3') {
        mode = Mode::checkInStation;
      } else {
        maintenanceMode = true;
        Serial.print("Special mode not implemented.");
      }
    } else {
      maintenanceMode = true;
      Serial.print("Mode not implemented.");
    }
  }
}

void setup() { 
  Serial.begin(115200);
  SPI.begin(); // Init SPI bus

  sprintf(clientid, "%06x", chipid);

  Wire.begin(D2,D3);
  lcd.init();   // initializing the LCD
  lcd.backlight(); // Enable or Turn On the backlight 
  lcd.clear();
  lcd.print(" Initialization ");
  lcd.setCursor(0,1);
  lcd.printf("    FW %s", FW_VERSION);
  delay(2000);
  lcd.createChar(0, lockChar);
  lcd.createChar(1, checkChar);
  lcd.createChar(2, skullChar);
  lcd.createChar(3, buttonChar);

  // Outputs
  pinMode(PIN_UNIT_SWITCH, OUTPUT);
  unitLock();

  // Uncomment below to reset saved WiFi networks and 
  // show the WiFi selection again
  //wifiManager.resetSettings();
  wifiManager.setTimeout(60*5);

  setupWiFi();

  // MQTT
  mqttClient.setServer(MQTT_SERVER, 1883);
  mqttClient.setCallback(callback);
  setupMQTT();
  String topic = String(MQTT_TOPIC) + String(clientid) + "/started";
  mqttClient.publish(topic.c_str(), clientid);


  // RFID Stuff
  lcd.clear(); lcd.print("   Init. RFID");
  rfid.PCD_Init(); // Init MFRC522 

  for (byte i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }

  Serial.println(F("This code scan the MIFARE Classsic NUID."));
  Serial.print(F("Using the following key:"));
  Serial.println(printHex(key.keyByte, MFRC522::MF_KEY_SIZE));

  // OTA
  lcd.clear(); lcd.print("   Init. OTA");
  ArduinoOTA.setHostname(clientid);
  ArduinoOTA.setPassword(FLASHPW);
  ArduinoOTA.setRebootOnSuccess(false);

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
    lcd.clear(); 
    lcd.print("UPGRADE COMPLETE");
    delay(5000);
    ESP.restart();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    lcd.clear(); 
    lcd.print("UPGRADE IN ");
    lcd.setCursor(0,1);
    lcd.printf("PROGRESS %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    timer_displayupdate.stop();
    lcd.clear(); 
    lcd.printf("OTA ERROR %u", error);
    Serial.printf("Error[%u]: ", error);
    delay(10000);
    ESP.restart();
  });

  timer_readcard.start();
  timer_displayupdate.start();

  idleDisplay();
}
 
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setupWiFi();
  }

  if (!mqttClient.connected()) {
    setupMQTT();
  }
  mqttClient.loop();

  if(mode == Mode::waitingForOTA) {
    ArduinoOTA.handle();
  }
  timer_readcard.update();
  timer_displayupdate.update();
  timer_machinerelock.update();
  timer_reportbroken.update();
  timer_quickdisplayclear.update();
  
}

void quickdisplayclear() {
  messageFlashLineTwo = "";
}

void reportBroken() {
  int	buttonValue = analogRead(PIN_USR_BUTTON);
	if (buttonValue>=1000 && buttonValue<=1024){
    Serial.print("reported broken");
    lcd.clear(); 
    lcd.print("   REPORTED!");
    lcd.setCursor(0,1);
    lcd.print(" RELEASE BUTTON ");

    String topic = String(MQTT_TOPIC) + String(clientid) + "/button_reported_broken";
    mqttClient.publish(topic.c_str(), clientid);
  }
  keepPressingToReport = false;
}

void pushToUnlockRequest() {
  String topic = String(MQTT_TOPIC) + String(clientid) + "/push_to_unlock";
  mqttClient.publish(topic.c_str(), clientid);
}

void unitUnlock() {
  digitalWrite(PIN_UNIT_SWITCH, HIGH);
}

void unitLock() {
  digitalWrite(PIN_UNIT_SWITCH, LOW);
}


void machinerelock() {
  timer_machinerelock.interval(0);
  timer_machinerelock.stop();
  lcd.backlight();
  unitLock();
  String topic = String(MQTT_TOPIC) + String(clientid) + "/state_relocked";
  mqttClient.publish(topic.c_str(), clientid);
}




void displayUpdate() {
  bool spinner = false;

  if (timer_machinerelock.state() == RUNNING && !keepPressingToReport) {
    displayLineOne = unitName;
  
    uint32_t sec_left = (unlockedTime-timer_machinerelock.elapsed())/1000; 
    uint16_t h; uint8_t m; uint8_t s; 
    uint32_t t = sec_left;

    s = t % 60;
    t = (t - s)/60;
    m = t % 60;
    t = (t - m)/60;
    h = t;

    if (h != 0) {
      displayLineTwo = String(h) + "h " + String(m) + "m " + String(s) + "s  rem.";
    } else {
      displayLineTwo = String(m) + "m " + String(s) + "s  rem.";
    }

    if(sec_left < 180) {
      if((displayBlinkingState) && (sec_left > 5)) {
        lcd.noBacklight();
      } else {
        lcd.backlight();
      }
      displayBlinkingState = !displayBlinkingState;
    }
  } else if(keepPressingToReport) {
    displayLineOne = "KEEP PRESSING TO";
    displayLineTwo = " REPORT BROKEN";
  } else if(messageFlashLineTwo != "") {
    displayLineTwo = messageFlashLineTwo;
  } else {
    spinner =  idleDisplay();
  }

  lcd.clear(); 
  lcd.print(displayLineOne);
  lcd.setCursor(0,1);
  
  displayLongReasonLength = displayLineTwo.length();
  if(maintenanceMode && (displayLongReasonLength > 16) && (timer_machinerelock.state() != RUNNING)) {
    String toShow;
    toShow = displayLineTwo.substring(displayScrollTick,displayScrollTick+16);
    displayScrollTick = displayScrollTick + 2;
      if(displayScrollTick>(displayLongReasonLength-16)) {
        displayScrollTick = 0;
      }
    lcd.print(toShow);
  } else {
    lcd.print(displayLineTwo);
  }
  
  lcd.setCursor(15, 0);
  switch (mode)
  {
  case Mode::requiresAuth:
    lcd.write(byte(0));
    break;
  case Mode::permUnlocked:
    lcd.write(byte(1));
    break;
  case Mode::pushToUnlock:
    lcd.write(byte(3));
    break;
  default:
    if (timer_machinerelock.state() == RUNNING)
    {
      lcd.write(byte(1));
    } else if (maintenanceMode) {
      lcd.write(byte(2));
    }
    break;
  }
    
  if(spinner) {
    lcd.setCursor(15,1);
    lcd.write(spinnerSymbols[spinnerTick%4]);
    spinnerTick = (spinnerTick+1) & 0b11;
  }

}

boolean idleDisplay() {
  if (unitName.equals(String("")) and mode != Mode::waitingForOTA) {
    displayLineOne = " Not configured";
    displayLineTwo = "     " + String(clientid);
  } else if (maintenanceMode) {
    displayLineOne = unitName;
    if(!maintenanceLongReason.equals(String(""))) {
      displayLineTwo = maintenanceLongReason;
      displayLongReasonLength = displayLineTwo.length();
      if (displayLongReasonLength < 16) {
        return true;
      } else {
        return false;
      }
    } else {
      displayLineTwo = "!SERVICE MODE!";
    }

  switch (mode)
  {
  case Mode::waitingForOTA:
    displayLineOne = "OTA MODE " + String(clientid);
    displayLineTwo = String((char*) ip2CharArray(WiFi.localIP()));
    break;
  case Mode::permUnlocked:
    displayLineOne = unitName;
    displayLineTwo = "Ready";
    break;
  case Mode::pushToUnlock:
    displayLineOne = unitName;
    displayLineTwo = "Push to unlock";
    break;
  case Mode::requiresAuth:
    displayLineOne = unitName;
    displayLineTwo = "ID to unlock";
    break;
  case Mode::checkInStation:
    displayLineOne = unitName;
    displayLineTwo = "Present ID";
    break;
  default:
    break;
  }

  return true;
}

void readCard() {
  if(!keepPressingToReport) {
    int	buttonValue = analogRead(PIN_USR_BUTTON);
    if (buttonValue>=1000 && buttonValue<=1024){
      if((millis() - lastDebounceTime) <= debounceDelay) {
         // Debounce funcs here
       } else {
        // Not debouncing
        if(timer_machinerelock.state() == RUNNING) {
          machinerelock();
        } else if (mode == Mode::pushToUnlock) {
          pushToUnlockRequest();
        } else if (maintenanceMode) {
          // nothing for now. 
        } else {
          Serial.print("Should send 'not working'");
          keepPressingToReport = true;
          timer_reportbroken.start();
        }
         lastDebounceTime = millis();
      }
    }
  }


  // Reset the loop if no new card present on the sensor/reader. This saves the entire process when idle.
  if ( ! rfid.PICC_IsNewCardPresent())
    return;

  // Verify if the NUID has been readed
  if ( ! rfid.PICC_ReadCardSerial())
    return;

  Serial.print(F("PICC type: "));
  MFRC522::PICC_Type piccType = rfid.PICC_GetType(rfid.uid.sak);
  Serial.println(rfid.PICC_GetTypeName(piccType));

  // Check is the PICC of Classic MIFARE type
  /** if (piccType != MFRC522::PICC_TYPE_MIFARE_MINI &&  
    piccType != MFRC522::PICC_TYPE_MIFARE_1K &&
    piccType != MFRC522::PICC_TYPE_MIFARE_4K) {
    Serial.println(F("Your tag is not of type MIFARE Classic."));
    return;
  } **/

  /**if (rfid.uid.uidByte[0] != nuidPICC[0] || 
    rfid.uid.uidByte[1] != nuidPICC[1] || 
    rfid.uid.uidByte[2] != nuidPICC[2] || 
    rfid.uid.uidByte[3] != nuidPICC[3] ) {
    Serial.print(F("Card NUID tag: ")); **/

    // Store NUID into nuidPICC array
    for (byte i = 0; i < 4; i++) {
      nuidPICC[i] = rfid.uid.uidByte[i];
    }
   
    Serial.print(printHex(rfid.uid.uidByte, rfid.uid.size));
    Serial.println();

    String topic = String(MQTT_TOPIC) + String(clientid) + String("/card_read");
    mqttClient.publish(topic.c_str(), String(printHex(rfid.uid.uidByte, rfid.uid.size)).c_str());
  
 /** } else Serial.println(F("Card read previously.")); **/

  // Halt PICC
  rfid.PICC_HaltA();

  // Stop encryption on PCD
  rfid.PCD_StopCrypto1();
}


/**
 * Helper routine to dump a byte array as hex values to Serial. 
 */
String printHex(byte *buffer, byte bufferSize) {
  String id = "";
  for (byte i = 0; i < bufferSize; i++) {
    id += buffer[i] < 0x10 ? "0" : "";
    id += String(buffer[i], HEX);
  }
  return id;
}

char* ip2CharArray(IPAddress IP) {
  static char a[16];
  sprintf(a, "%d.%d.%d.%d", IP[0], IP[1], IP[2], IP[3]);
  return a;
}
