/**
   A sketch that searches for a compatible Airthings device and 
   publishes the radon level, temperature, and humidity to an MQTT 
   server.

   The sketch was created with the intention to allow Airthings devices
   to cheaply integrate with Home Assistant.

   To use:
   (1) Set up your Airthings following the manufacter's instructions.
   (2) Install the PunSubClient library (https://pubsubclient.knolleary.net/).
   (3) Set your WiFi credentials below.
   (4) Set your MQTT server/credentials below.
   (5) Update the published topics below (if desired).
   (6) Flash to any ESP32 board.
   (7) Watch the Serial output to make sure it works.

   * The library runs once an hour to take a reading and deep sleeps in 
     between, so feasibly this could run on a battery for a very long time.
   * The library will attempt to find any airthings device to read from, 
     picking the first it finds.  The Airthings BLE API is unauthenticated 
     so no device configuration or pairing is necessary on the Airthings.
   * The library will not interfere with your Airthings' normal upload to a 
     phone/cloud.
   * If it fails to read, it will attempt again after 30 seconds instead.
   * I only have an Airthings Wave to test this with, though presumably it 
     would also work with the Wave Plus.
   * The ESP32's bluetooth stack is a little unstable IMHO so expect this to
     hang for a few minutes, restart prematurely, and report errors often.
*/
#include <FS.h>
#include <BLEDevice.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <PubSubClient.h>         //https://github.com/knolleary/pubsubclient
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager/archive/development.zip
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

#define HOSTNAME "airthings_client"
// WiFi credentials.
// #define WIFI_SSID "YOUR SSID"
// #define WIFI_PASS "YOUR PASSWORD"

// MQTT Settings.
char mqtt_server[40] = "10.0.0.30";
char mqtt_username[40] = "";
char mqtt_password[40] = "";
char mqtt_port[6] = "1883";
char mqtt_client_name[100] = HOSTNAME;

// The MQTT topic to publish a 24 hour average of radon levels to.
#define TOPIC_RADON_SHORT_TERM "myHome/airthings/stat/radonShortTermAvg"
// The MQTT topic to publish the lifetime radon average to.  Documentation
// says this will be the average ever since the batteries were removed.
#define TOPIC_RADON_LONG_TERM "myHome/airthings/stat/radonLongTermAvg"
// Topics for temperature and humidity.
#define TOPIC_TEMPERATURE "myHome/airthings/stat/temperature"
#define TOPIC_HUMIDITY "myHome/airthings/stat/humidity"
#define TOPIC_CO2 "myHome/airthings/stat/co2"
#define TOPIC_ATM_PRESSURE "myHome/airthings/stat/atmPressure"
#define TOPIC_TVOC "myHome/airthings/stat/tvoc"

// Unlikely you'll need to chnage any of the settings below.

// The time to take between readings.  One hour has worked pretty well for me.  
// Since the device only gives us the 24hr average, more frequent readings 
// probably wouldn't be useful, run the airthings battery down, and risk 
// interfering with the "normal" mechanism Airthings uses to publish info
// to your phone.
#define READ_WAIT_SECONDS 60*60 // One hour

// If taking a reading fails for any reason (BLE is pretty flaky...) then
// the ESP will sleep for this long before retrying.
#define READ_WAIT_RETRY_SECONDS 30

// How long the ESP will wait to connect to WiFi, scan for 
// Airthings devices, etc.
#define CONNECT_WAIT_SECONDS 30

// Some useful constants.
#define uS_TO_S_FACTOR 1000000
#define SECONDS_TO_MILLIS 1000
#define BECQUERELS_M2_TO_PICOCURIES_L 37.0
#define DOT_PRINT_INTERVAL 50

// The hard-coded uuid's airthings uses to advertise itself and its data.
static BLEUUID serviceUUID("b42e1c08-ade7-11e4-89d3-123b93f75cba");
static BLEUUID UUID("b42e2a68-ade7-11e4-89d3-123b93f75cba");

bool getAndRecordReadings(BLEAddress pAddress) {
  Serial.println();
  Serial.println("Connecting...");
  BLEClient* client = BLEDevice::createClient();

  // Connect to the remove BLE Server.
  if (!client->connect(pAddress)) {
    Serial.println("Failed to connect.");
    return false;
  }

  Serial.println("Connected!");
  // Obtain a reference to the service we are after in the remote BLE server.
  Serial.println("Retrieving service reference...");
  BLERemoteService* pRemoteService = client->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.print("Airthings refused its service UUID.");
    client->disconnect();
    return false;
  }

  // Get references to our characteristics
  Serial.println("Reading data...");
  BLERemoteCharacteristic* sensorCharacteristic = pRemoteService->getCharacteristic(UUID);

  if (sensorCharacteristic == nullptr) {
    Serial.print("Failed to read from the device!");
    return false;
  }

  float temperature = 0;
  float humidity = 0;
  float atm_pressure = 0;
  float tvoc = 0;
  float co2 = 0;

  // The radon values are reported in terms of 
  float radon = 0;
  float radonLongterm = 0;

  if(sensorCharacteristic->canRead()) {
      std::string value = sensorCharacteristic->readValue();
      uint32_t first32bytes = sensorCharacteristic->readUInt32();
      uint8_t firstByte = sensorCharacteristic->readUInt8();
      uint8_t* rawData = sensorCharacteristic->readRawData();
      
      Serial.print("The characteristic value was: ");
      Serial.println(first32bytes);
      Serial.printf("sensorReading:",sensorCharacteristic);
      // Serial.println(value.c_str());
      Serial.print("Value length: ");
      Serial.println(value.length());

      Serial.print("firstByte");
      Serial.println(firstByte);

      // Serial.print("First byte");
      Serial.print("Data: ");
      for(int i=0;i<value.length();i++){
        Serial.print((uint8_t)value[i],HEX);
        Serial.print(" ");
      }
      Serial.println();
      for(int i=0;i<value.length();i++){
        Serial.print((uint8_t)value[i]);
        Serial.print(" ");
      }
      Serial.println();

      Serial.println("Humidity: ");
      humidity = (uint8_t)value[1]/2;
      
      Serial.println("Radon short term: ");
      radon = (uint8_t)value[5] * 256 + (uint8_t)value[4]; 
      Serial.println((uint16_t)radon);

      Serial.println("Radon long term: ");
      radonLongterm = (uint8_t)value[7] * 256 + (uint8_t)value[6]; 
      Serial.println((uint16_t)radonLongterm);

      Serial.println("Temperature: ");
      temperature = ((uint8_t)value[9] * 256 + (uint8_t)value[8]) / 100; 
      Serial.println((uint16_t)temperature);
      
      Serial.println("Atmos: ");
      atm_pressure = ((uint8_t)value[11] * 256 + (uint8_t)value[10]) / 50; 
      Serial.println((uint16_t)atm_pressure);
      
      Serial.println("CO2: ");
      co2 = ((uint8_t)value[13] * 256 + (uint8_t)value[12]) * 1.0; 
      Serial.println((uint16_t)co2);

      Serial.println("VOC: ");
      tvoc = ((uint8_t)value[15] * 256 + (uint8_t)value[14]) * 1.0; 
      Serial.println((uint16_t)tvoc);

      //Serial.print("value size = ");
      // Serial.println(sensorCharacteristic->;
    }

  
  client->disconnect();
  
  Serial.printf("Temperature: %f\n", temperature);
  Serial.printf("Humidity: %f\n", humidity);
  Serial.printf("Radon 24hr average: %f\n", radon);
  Serial.printf("TVOC: %f\n", tvoc);
  Serial.printf("Atm pressure: %f\n", atm_pressure);
  Serial.printf("CO2: %f\n", co2);
  Serial.printf("Radon Lifetime average: %f\n", radonLongterm);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() < start + CONNECT_WAIT_SECONDS * SECONDS_TO_MILLIS) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Failed to connect to wifi");
    return false;
  }

  
  // Connect and publish to MQTT.
  WiFiClient espClient;
  PubSubClient mqtt(espClient);
  mqtt.setServer(mqtt_server, atoi(mqtt_port));
  if (!mqtt.connect(mqtt_client_name, mqtt_username, mqtt_password) ||
      !mqtt.publish(TOPIC_TVOC, String(tvoc).c_str()) ||
      !mqtt.publish(TOPIC_CO2, String(co2).c_str()) ||
      !mqtt.publish(TOPIC_ATM_PRESSURE, String(atm_pressure).c_str()) ||
      !mqtt.publish(TOPIC_RADON_SHORT_TERM, String(radon).c_str()) ||
      !mqtt.publish(TOPIC_RADON_LONG_TERM, String(radonLongterm).c_str()) ||
      !mqtt.publish(TOPIC_TEMPERATURE, String(temperature).c_str()) ||
      !mqtt.publish(TOPIC_HUMIDITY, String(humidity).c_str())) {
    Serial.println("Unable to connect/publish to mqtt server.");
    return false;
  }
  return true;
}

// The bluetooth stack takes a callback when scannign for devices.  The first Airthings device it finds it will record in pServerAddress.
class FoundDeviceCallback: public BLEAdvertisedDeviceCallbacks {
  public: 
  BLEAddress* address;
  bool found = false;
  bool foundAirthings() {
    return found;
  }
  BLEAddress getAddress() {
    return *address;
  }
  void onResult(BLEAdvertisedDevice device) {
    // We have found a device, see if it has the Airthings service UUID
    if (device.haveServiceUUID() && device.getServiceUUID().equals(serviceUUID)) {
      Serial.print("Found our device: ");
      Serial.println(device.toString().c_str());
      device.getScan()->stop();
      address = new BLEAddress(device.getAddress());
      found = true;
    }
  }
};

// /****************************  Read/Write MQTT Settings from SPIFFs ****************************************/

bool readConfigFS()
{
  //if (resetsettings) { SPIFFS.begin(); SPIFFS.remove("/config.json"); SPIFFS.format(); delay(1000);}
  if (SPIFFS.exists("/config.json"))
  {
    Serial.print(F("Read cfg: "));
    File configFile = SPIFFS.open("/config.json", "r");
    if (configFile)
    {
      size_t size = configFile.size(); // Allocate a buffer to store contents of the file.
      std::unique_ptr<char[]> buf(new char[size]);
      configFile.readBytes(buf.get(), size);
      StaticJsonDocument<200> jsonBuffer;
      DeserializationError error = deserializeJson(jsonBuffer, buf.get());
      if (!error)
      {
        JsonObject json = jsonBuffer.as<JsonObject>();
        serializeJson(json, Serial);
        strcpy(mqtt_server, json["mqtt_server"]);
        strcpy(mqtt_port, json["mqtt_port"]);
        strcpy(mqtt_username, json["mqtt_username"]);
        strcpy(mqtt_password, json["mqtt_password"]);
        return true;
      }
      else
        Serial.println(F("Failed to parse JSON!"));
    }
    else
      Serial.println(F("Failed to open \"/config.json\""));
  }
  else
    Serial.println(F("Couldn't find \"/config.json\""));
  return false;
}

bool writeConfigFS()
{
  Serial.print(F("Saving /config.json: "));
  StaticJsonDocument<200> jsonBuffer;
  JsonObject json = jsonBuffer.to<JsonObject>();
  json["mqtt_server"] = mqtt_server;
  json["mqtt_port"] = mqtt_port;
  json["mqtt_username"] = mqtt_username;
  json["mqtt_password"] = mqtt_password;
  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile)
  {
    Serial.println(F("failed to open config file for writing"));
    return false;
  }
  serializeJson(json, Serial);
  serializeJson(json, configFile);
  configFile.close();
  Serial.println(F("ok!"));
  return true;
}

// /*****************  Read SPIFFs values *****************************/
void listDir(fs::FS &fs, const char *dirname, uint8_t levels)
{
  Serial.printf("Listing directory: %s\n", dirname);

  File root = fs.open(dirname);
  if (!root)
  {
    Serial.println("Failed to open directory");
    return;
  }
  if (!root.isDirectory())
  {
    Serial.println("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
      Serial.print("  DIR : ");
      Serial.print(file.name());
      time_t t = file.getLastWrite();
      struct tm *tmstruct = localtime(&t);
      Serial.printf("  LAST WRITE: %d-%02d-%02d %02d:%02d:%02d\n", (tmstruct->tm_year) + 1900, (tmstruct->tm_mon) + 1, tmstruct->tm_mday, tmstruct->tm_hour, tmstruct->tm_min, tmstruct->tm_sec);
      if (levels)
      {
        listDir(fs, file.name(), levels - 1);
      }
    }
    else
    {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("  SIZE: ");
      Serial.print(file.size());
      time_t t = file.getLastWrite();
      struct tm *tmstruct = localtime(&t);
      Serial.printf("  LAST WRITE: %d-%02d-%02d %02d:%02d:%02d\n", (tmstruct->tm_year) + 1900, (tmstruct->tm_mon) + 1, tmstruct->tm_mday, tmstruct->tm_hour, tmstruct->tm_min, tmstruct->tm_sec);
    }
    file = root.openNextFile();
  }
  Serial.println(F("SPIFFs started"));
  Serial.println(F("---------------------------"));
}

// /*****************  WiFiManager *****************************/
bool shouldSaveConfig = false;

void saveConfigCallback()
{
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

// /*****************  Setup *****************************/
void setup() {
  Serial.begin(115200);

  Serial.println(F("---------------------------"));
  Serial.println(F("Starting SPIFFs"));

  if (SPIFFS.begin(true)) //format SPIFFS if needed
  {
    listDir(SPIFFS, "/", 0);
  }

  if (readConfigFS())
    Serial.println(F(" yay!"));

  char NameChipId[64] = {0}, chipId[9] = {0};
  WiFi.mode(WIFI_STA); // Make sure you're in station mode
  WiFi.setHostname(HOSTNAME);
  snprintf(chipId, sizeof(chipId), "%08x", (uint32_t)ESP.getEfuseMac());
  snprintf(NameChipId, sizeof(NameChipId), "%s_%08x", HOSTNAME, (uint32_t)ESP.getEfuseMac());
  WiFi.setHostname(const_cast<char *>(NameChipId));
  WiFiManager wifiManager;

  WiFiManagerParameter custom_mqtt_server("mqtt_server", "mqtt server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_username("mqtt_username", "mqtt username", mqtt_username, 40, " maxlength=31");
  WiFiManagerParameter custom_mqtt_password("mqtt_password", "mqtt password", mqtt_password, 40, " maxlength=31 type='password'");
  WiFiManagerParameter custom_mqtt_port("mqtt_port", "mqtt port", mqtt_port, 6);

  wifiManager.setSaveConfigCallback(saveConfigCallback);

  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_username);
  wifiManager.addParameter(&custom_mqtt_password);

  if (!wifiManager.autoConnect(HOSTNAME))
  {
    Serial.println(F("failed to connect and hit timeout"));
    delay(3000);
    ESP.restart();
    delay(5000);
  }
  Serial.println(F("connected!"));

  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_username, custom_mqtt_username.getValue());
  strcpy(mqtt_password, custom_mqtt_password.getValue());

  if (shouldSaveConfig)
  {
    writeConfigFS();
    shouldSaveConfig = false;
  }

  byte mac[6];
  WiFi.macAddress(mac);
  sprintf(mqtt_client_name, "%02X%02X%02X%02X%02X%02X%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], HOSTNAME);

  // Scan for an Airthings device.
  Serial.println("Scanning for airthings devices");
  BLEDevice::init("");
  BLEScan* pBLEScan = BLEDevice::getScan();
  FoundDeviceCallback* callback = new FoundDeviceCallback();
  pBLEScan->setAdvertisedDeviceCallbacks(callback);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(30);

  unsigned long timeToSleep = 0;
  if (!callback->foundAirthings()) {
    // We timed out looking for an Airthings.
    Serial.printf("\nFAILED to find any Airthings devices. Sleeping for %i seconds before retrying.\n", READ_WAIT_RETRY_SECONDS);
    timeToSleep = READ_WAIT_RETRY_SECONDS;
  } else if (getAndRecordReadings(callback->getAddress())) {
    Serial.printf("\nReading complete. Sleeping for %i seconds before taking another reading.\n", READ_WAIT_SECONDS);
    timeToSleep = READ_WAIT_SECONDS;
  } else {
    Serial.printf("\nReading FAILED. Sleeping for %i seconds before retrying.\n", READ_WAIT_RETRY_SECONDS);
    timeToSleep = READ_WAIT_RETRY_SECONDS;
  }
  Serial.flush();
  esp_sleep_enable_timer_wakeup(timeToSleep * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

void loop() {
  // We should never reach here.
  delay(1);
}
