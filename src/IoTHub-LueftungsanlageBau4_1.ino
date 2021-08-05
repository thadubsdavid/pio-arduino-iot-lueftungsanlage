/*
// Read temperature and humidity data from an Arduino MKR1000 or MKR1010 device using a DHT11/DHT22 sensor.
// The data is then sent to Azure IoT Central for visualizing via MQTT
//
// See the readme.md for details on connecting the sensor and setting up Azure IoT Central to recieve the data.
*/

#include <stdarg.h>
#include <SPI.h>
/*
// some test 
// are we compiling against the Arduino MKR1000
#if defined(ARDUINO_SAMD_MKR1000) && !defined(WIFI_101)
#include <WiFi101.h>
#define DEVICE_NAME "Arduino MKR1000"
#endif
*/
// are we compiling against the Arduino MKR1010
#ifdef ARDUINO_SAMD_MKRWIFI1010
#include <WiFiNINA.h>
#define DEVICE_NAME "Arduino MKR1010"
#endif

#include <WiFiUdp.h>
#include <RTCZero.h>
// #include <SimpleDHT.h>

/*  You need to go into this file and change this line from:
      #define MQTT_MAX_PACKET_SIZE 128
    to:
      #define MQTT_MAX_PACKET_SIZE 2048
*/
#include <PubSubClient.h>

// change the values for Wi-Fi, Azure IoT Central device, and DHT sensor in this file
#include "./configure.h"

// this is an easy to use NTP Arduino library by Stefan Staub - updates can be found here https://github.com/sstaub/NTP
#include "./ntp.h"
#include "./sha256.h"
#include "./base64.h"
#include "./parson.h"
#include "./morse_code.h"
#include "./utils.h"

#include <avr/dtostrf.h>

// Librarys
//*************************************************************
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <ArduinoJson.h>

// Funktion Header
//*************************************************************
// void getTemperature(int Rohwerte);
// void getBME(Adafruit_BME280 bme);
// int getHubHostName(char *scopeId, char* deviceId, char* key, char *hostName);

// Variablen deklaration
//*************************************************************
float temperatureK;
float temperatureInside;
float temperatureOutside;
float humidityInside;
float humidityOutside;
float pressureInside;
float pressureOutside;

int reconnectCounter;

// Instanzen BME280 (Barometric pressure/relative humidity/temperature)
//*************************************************************
Adafruit_BME280 bmeInside, bmeOutside;

enum workingMode {simulated, sensors}; 

#if defined READSENSORS
workingMode workingMode = sensors;
#else
workingMode workingMode = simulated;
#endif

String iothubHost;
String deviceId;
String sharedAccessKey;

WiFiSSLClient wifiClient;
PubSubClient *mqtt_client = NULL;

bool timeSet = false;
bool wifiConnected = false;
bool mqttConnected = false;

time_t this_second = 0;
time_t checkTime = 1300000000;

#define TELEMETRY_SEND_INTERVAL 5000  // telemetry data sent every 5 seconds
#define PROPERTY_SEND_INTERVAL  15000 // property data sent every 15 seconds
#define SENSOR_READ_INTERVAL  2500    // read sensors every 2.5 seconds

long lastTelemetryMillis = 0;
long lastPropertyMillis = 0;
long lastSensorReadMillis = 0;

float tempValue = 0.0;
float humidityValue = 0.0;
int dieNumberValue = 1;

// MQTT publish topics
static const char PROGMEM IOT_EVENT_TOPIC[] = "devices/{device_id}/messages/events/$.ct=application%2Fjson&$.ce=utf-8";
static const char PROGMEM IOT_TWIN_REPORTED_PROPERTY[] = "$iothub/twin/PATCH/properties/reported/?$rid={request_id}";
static const char PROGMEM IOT_TWIN_REQUEST_TWIN_TOPIC[] = "$iothub/twin/GET/?$rid={request_id}";
static const char PROGMEM IOT_DIRECT_METHOD_RESPONSE_TOPIC[] = "$iothub/methods/res/{status}/?$rid={request_id}";

// MQTT subscribe topics
static const char PROGMEM IOT_TWIN_RESULT_TOPIC[] = "$iothub/twin/res/#";
static const char PROGMEM IOT_TWIN_DESIRED_PATCH_TOPIC[] = "$iothub/twin/PATCH/properties/desired/#";
static const char PROGMEM IOT_C2D_TOPIC[] = "devices/{device_id}/messages/devicebound/#";
static const char PROGMEM IOT_DIRECT_MESSAGE_TOPIC[] = "$iothub/methods/POST/#";

int requestId = 0;
int twinRequestId = -1;

// create a WiFi UDP object for NTP to use
WiFiUDP wifiUdp;
// create an NTP object
NTP ntp(wifiUdp);
// Create an rtc object
RTCZero rtc;

#include "./iotc_dps.h"

// get the time from NTP and set the real-time clock on the MKR10x0
void getTime() {
  Serial.println(F("Getting the time from time service: "));


  ntp.ruleDST("CEST", Last, Sun, Mar, 2, 120); // last sunday in march 2:00, timetone +120min (+1 GMT + 1h summertime offset);
  ntp.ruleSTD("CET", Last, Sun, Oct, 3, 60); // last sunday in october 3:00, timezone +60min (+1 GMT);
  ntp.begin();
  ntp.update();
  Serial.print(F("Current time: "));
  Serial.print(ntp.formattedTime("%d. %B %Y - "));
  Serial.println(ntp.formattedTime("%A %T"));

  rtc.begin();
  rtc.setEpoch(ntp.epoch());
  timeSet = true;
}

void acknowledgeSetting(const char* propertyKey, const char* propertyValue, int version) {
  // for IoT Central need to return acknowledgement
  const static char* PROGMEM responseTemplate = "{\"%s\":{\"value\":%s,\"statusCode\":%d,\"status\":\"%s\",\"desiredVersion\":%d}}";
  char payload[1024];
  sprintf(payload, responseTemplate, propertyKey, propertyValue, 200, F("completed"), version);
  Serial_printf("Sending acknowledgement: %s\n\n", payload);
  String topic = (String)IOT_TWIN_REPORTED_PROPERTY;
  char buff[20];
  topic.replace(F("{request_id}"), itoa(requestId, buff, 10));
  mqtt_client->publish(topic.c_str(), payload);
  requestId++;
}

void handleDirectMethod(String topicStr, String payloadStr) {
    String msgId = topicStr.substring(topicStr.indexOf("$RID=") + 5);
    String methodName = topicStr.substring(topicStr.indexOf(F("$IOTHUB/METHODS/POST/")) + 21, topicStr.indexOf("/?$"));
    Serial_printf((char*)F("Direct method call:\n\tMethod Name: %s\n\tParameters: %s\n"), methodName.c_str(), payloadStr.c_str());
    if (strcmp(methodName.c_str(), "ECHO") == 0) {
        // acknowledge receipt of the command
        String response_topic = (String)IOT_DIRECT_METHOD_RESPONSE_TOPIC;
        char buff[20];
        response_topic.replace(F("{request_id}"), msgId);
        response_topic.replace(F("{status}"), F("200"));  //OK
        mqtt_client->publish(response_topic.c_str(), "");

        // output the message as morse code
        JSON_Value *root_value = json_parse_string(payloadStr.c_str());
        JSON_Object *root_obj = json_value_get_object(root_value);
        const char* msg = json_object_get_string(root_obj, "displayedValue");
        morse_encodeAndFlash(msg);
        json_value_free(root_value);
    }
}

void handleCloud2DeviceMessage(String topicStr, String payloadStr) {
    Serial_printf((char*)F("Cloud to device call:\n\tPayload: %s\n"), payloadStr.c_str());
}

void handleTwinPropertyChange(String topicStr, String payloadStr) {
    // read the property values sent using JSON parser
    JSON_Value *root_value = json_parse_string(payloadStr.c_str());
    JSON_Object *root_obj = json_value_get_object(root_value);
    const char* propertyKey = json_object_get_name(root_obj, 0);
    double propertyValueNum;
    double propertyValueBool;
    double version;
    if (strcmp(propertyKey, "fanSpeed") == 0 || strcmp(propertyKey, "setVoltage") == 0 || strcmp(propertyKey, "setCurrent") == 0 || strcmp(propertyKey, "activateIR") == 0) {
        JSON_Object* valObj = json_object_get_object(root_obj, propertyKey);
        if (strcmp(propertyKey, "activateIR") == 0) {
            propertyValueBool = json_object_get_boolean(valObj, "value");
        } else {
            propertyValueNum = json_object_get_number(valObj, "value");
        }
        version = json_object_get_number(root_obj, "$version");
        char propertyValueStr[8];
        if (strcmp(propertyKey, "activateIR") == 0) {
            if (propertyValueBool) {
                strcpy(propertyValueStr, "true");
            } else {
                strcpy(propertyValueStr, "false");
            }
        } else {
            itoa(propertyValueNum, propertyValueStr, 10);
        }
        Serial_printf("\n%s setting change received with value: %s\n", propertyKey, propertyValueStr);
        acknowledgeSetting(propertyKey, propertyValueStr, version);
    }
    json_value_free(root_value);
}

// callback for MQTT subscriptions
void callback(char* topic, byte* payload, unsigned int length) {
    String topicStr = (String)topic;
    topicStr.toUpperCase();
    String payloadStr = (String)((char*)payload);
    payloadStr.remove(length);

    if (topicStr.startsWith(F("$IOTHUB/METHODS/POST/"))) { // direct method callback
        handleDirectMethod(topicStr, payloadStr);
    } else if (topicStr.indexOf(F("/MESSAGES/DEVICEBOUND/")) > -1) { // cloud to device message
        handleCloud2DeviceMessage(topicStr, payloadStr);
    } else if (topicStr.startsWith(F("$IOTHUB/TWIN/PATCH/PROPERTIES/DESIRED"))) {  // digital twin desired property change
        handleTwinPropertyChange(topicStr, payloadStr);
    } else if (topicStr.startsWith(F("$IOTHUB/TWIN/RES"))) { // digital twin response
        int result = atoi(topicStr.substring(topicStr.indexOf(F("/RES/")) + 5, topicStr.indexOf(F("/?$"))).c_str());
        int msgId = atoi(topicStr.substring(topicStr.indexOf(F("$RID=")) + 5, topicStr.indexOf(F("$VERSION=")) - 1).c_str());
        if (msgId == twinRequestId) {
            // twin request processing
            twinRequestId = -1;
            // output limited to 128 bytes so this output may be truncated
            Serial_printf((char*)F("Current state of device twin:\n\t%s"), payloadStr.c_str());
            Serial.println();
        } else {
            if (result >= 200 && result < 300) {
                Serial_printf((char*)F("--> IoT Hub acknowledges successful receipt of twin property: %d\n"), msgId);
            } else {
                Serial_printf((char*)F("--> IoT Hub could not process twin property: %d, error: %d\n"), msgId, result);
            }
        }
    } else { // unknown message
        Serial_printf((char*)F("Unknown message arrived [%s]\nPayload contains: %s"), topic, payloadStr.c_str());
    }
}

// connect to Azure IoT Hub via MQTT
void connectMQTT(String deviceId, String username, String password) {
    mqtt_client->disconnect();

    Serial.println(F("Starting IoT Hub connection"));
    int retry = 0;
    while(retry < 10 && !mqtt_client->connected()) {     
        if (mqtt_client->connect(deviceId.c_str(), username.c_str(), password.c_str())) {
                Serial.println(F("===> mqtt connected"));
                mqttConnected = true;
        } else {
            Serial.print(F("---> mqtt failed, rc="));
            Serial.println(mqtt_client->state());
            delay(2000);
            retry++;
        }
    }
}

// create an IoT Hub SAS token for authentication
String createIotHubSASToken(char *key, String url, long expire){
    url.toLowerCase();
    String stringToSign = url + "\n" + String(expire);
    int keyLength = strlen(key);

    int decodedKeyLength = base64_dec_len(key, keyLength);
    char decodedKey[decodedKeyLength];

    base64_decode(decodedKey, key, keyLength);

    Sha256 *sha256 = new Sha256();
    sha256->initHmac((const uint8_t*)decodedKey, (size_t)decodedKeyLength);
    sha256->print(stringToSign);
    char* sign = (char*) sha256->resultHmac();
    int encodedSignLen = base64_enc_len(HASH_LENGTH);
    char encodedSign[encodedSignLen];
    base64_encode(encodedSign, sign, HASH_LENGTH);
    delete(sha256);

    return (char*)F("SharedAccessSignature sr=") + url + (char*)F("&sig=") + urlEncode((const char*)encodedSign) + (char*)F("&se=") + String(expire);
}

// reads the value from the DHT sensor if present else generates a random value
void readSensors() {
    dieNumberValue = random(1, 7);

    #if defined READSENSORS
    while(!bmeInside.begin(0x77)) {
        Serial.println("Could not find BME280 sensor Inside! Check wiring and I2C-address");
        delay(1000);
    }
    while(!bmeOutside.begin(0x76)) {
        Serial.println("Could not find BME280 sensor Outside! Check wiring and I2C-address");
        delay(1000);
    }
    temperatureOutside = bmeOutside.readTemperature();
    temperatureInside = bmeInside.readTemperature();
    humidityInside = bmeInside.readHumidity();
    humidityOutside = bmeOutside.readHumidity();
    pressureInside = bmeInside.readPressure();
    pressureOutside = bmeOutside.readPressure();
    temperatureK = ((( analogRead(TC_PIN) * (AREF / (pow(2, ADC_RESOLUTION) - 1))) - 1.25) / 0.005);
    #else
    humidityValue = random(0, 9999) / 100.0;
    temperatureOutside = random(0, 7500) / 100.0;
    temperatureInside = random(0, 7500) / 100.0;
    humidityInside = random(0, 9999) / 100.0;
    humidityOutside = random(0, 9999) / 100.0;
    pressureInside = random(98000, 99999) / 100.0;
    pressureOutside = random(98000, 99999) / 100.0;
    temperatureK = random(0, 7500) / 100.0;
    #endif
}

void reconnectWiFi()
{
  int statusWiFi = WiFi.status();

  // attempt to connect to Wifi network:
  while ( statusWiFi != WL_CONNECTED) {
    Serial.print("Attempting to connect to SSID: ");
    Serial.println(wifi_ssid);
    Serial.print("Status: ");
    Serial.println(statusWiFi);

    // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
    statusWiFi = WiFi.begin(wifi_ssid, wifi_password);
    reconnectCounter ++;
    if( statusWiFi != WL_CONNECTED )
    {
      Serial.print("Connect Fail - call end() wait 5s status:");
      Serial.println(statusWiFi);      

      Serial.print("Reason code: ");
      Serial.println(WiFi.reasonCode());

      // Connect was not successful... retry
      WiFi.end();
      delay( 5000 );
    } else {
      // Give it 1s connected...
      delay( 1000 );
    }
    
  }
}

// Setup Funktion
//*************************************************************
void setup() {
  Serial.begin(115200);

  // Verbindungsstatus BME280
  #if defined READSENSORS
  	while(!bmeInside.begin(0x77)) {
      Serial.println("Could not find BME280 sensor Inside! Check wiring and I2C-address");
      delay(1000);
    }
  	while(!bmeOutside.begin(0x76)) {
      Serial.println("Could not find BME280 sensor Outside! Check wiring and I2C-address");
      delay(1000);
    }
    // uncomment this line to add a small delay to allow time for connecting serial moitor to get full debug output
    delay(5000);
  #endif

  Serial_printf((char*)F("Hello, starting up the %s device\n"), DEVICE_NAME);

  // seed pseudo-random number generator for die roll and simulated sensor values
  randomSeed(millis());

  // attempt to connect to Wifi network:
  Serial.print((char*)F("WiFi Firmware version is "));
  Serial.println(WiFi.firmwareVersion());
  int status = WL_IDLE_STATUS;
  while ( status != WL_CONNECTED) {
      Serial_printf((char*)F("Attempting to connect to Wi-Fi SSID: %s \n"), wifi_ssid);
      // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
      status = WiFi.begin(wifi_ssid, wifi_password);
      delay(1000);
  }

  // get current UTC time
  getTime();

  Serial.println("Getting IoT Hub host from Azure IoT DPS");
  deviceId = iotc_deviceId;
  sharedAccessKey = iotc_deviceKey;
  // char hostName[64] = {0};
  //getHubHostName((char*)iotc_scopeId, (char*)iotc_deviceId, (char*)iotc_deviceKey, hostName);
  //iothubHost = hostName;
  iothubHost = iotc_scopeId;
    
  // create SAS token and user name for connecting to MQTT broker
  String url = iothubHost + urlEncode(String((char*)F("/devices/") + deviceId).c_str());
  char *devKey = (char *)sharedAccessKey.c_str();
  long expire = rtc.getEpoch() + 864000;
  String sasToken = createIotHubSASToken(devKey, url, expire);
  String username = iothubHost + "/" + deviceId + (char*)F("/api-version=2016-11-14");

  // connect to the IoT Hub MQTT broker
  wifiClient.connect(iothubHost.c_str(), 8883);
  mqtt_client = new PubSubClient(iothubHost.c_str(), 8883, wifiClient);
  connectMQTT(deviceId, username, sasToken);
  mqtt_client->setCallback(callback);

  // add subscriptions
  mqtt_client->subscribe(IOT_TWIN_RESULT_TOPIC);  // twin results
  mqtt_client->subscribe(IOT_TWIN_DESIRED_PATCH_TOPIC);  // twin desired properties
  String c2dMessageTopic = IOT_C2D_TOPIC;
  c2dMessageTopic.replace(F("{device_id}"), deviceId);
  mqtt_client->subscribe(c2dMessageTopic.c_str());  // cloud to device messages
  mqtt_client->subscribe(IOT_DIRECT_MESSAGE_TOPIC); // direct messages

  // request full digital twin update
  String topic = (String)IOT_TWIN_REQUEST_TWIN_TOPIC;
  char buff[20];
  topic.replace(F("{request_id}"), itoa(requestId, buff, 10));
  twinRequestId = requestId;
  requestId++;
  mqtt_client->publish(topic.c_str(), "");

  // initialize timers
  lastTelemetryMillis = millis();
  lastPropertyMillis = millis();
}

// Loop Funktion
//*************************************************************
void loop() {
  // give the MQTT handler time to do it's thing
  mqtt_client->loop();

  // read the sensor values
  if (millis() - lastSensorReadMillis > SENSOR_READ_INTERVAL) {
    readSensors();

    // Auslesen des Tempsensor Typ K
    // getTemperature(analogRead(TC_PIN));
    // Auslesen des BME Sensors
    // getBME(bmeInside);
    // getBME(bmeOutside);   

    lastSensorReadMillis = millis();
  }
    
  // send telemetry values every 5 seconds
  if (mqtt_client->connected() && millis() - lastTelemetryMillis > TELEMETRY_SEND_INTERVAL) {
    reconnectWiFi();
    DynamicJsonDocument doc(2048);
    Serial.println(F("Sending telemetry ..."));
    ntp.update();
    String topic = (String)IOT_EVENT_TOPIC;
    topic.replace(F("{device_id}"), deviceId);
    char buffer[2048];
    doc["TempInside"]        = temperatureInside;
    doc["HumidityInside"]    = humidityInside;
    doc["PressureInside"]    = pressureInside;
    doc["TempOutside"]       = temperatureOutside;
    doc["HumidityOutside"]   = humidityOutside;
    doc["PressureOutside"]   = pressureOutside;
    doc["TempTypK"]          = temperatureK;
    doc["DeviceID"]          = deviceId;
    // String timestamp = ntp.formattedTime("%Y-%m-%dT%H:%M:%S");
    // Serial.println(timestamp);
    // doc["DeviceTimestamp"]   = timestamp;
    char dt[16];
    char tm[16];
    // String dt;
    // String tm;
    sprintf(dt, "20%02d-%02d-%02dT", rtc.getYear(),rtc.getMonth(),rtc.getDay());
    //dt = rtc.getYear() + "-" + rtc.getMonth() + "-" + rtc.getDay() + "T";
    //tm.c_str() = rtc.getHours() + ":" + rtc.getMinutes() + ":" + rtc.getSeconds();
    /*strcat(dt, rtc.getYear());
    strcat(dt, "-");
    strcat(dt, rtc.getMonth());
    strcat(dt, "-");
    strcat(dt, rtc.getDay());
    strcat(dt, "T");
    strcat(tm, rtc.getHours());
    strcat(dt, ":");
    strcat(tm, rtc.getMinutes());
    strcat(dt, ":");
    strcat(tm, rtc.getSeconds());*/
    sprintf(tm, "%02d:%02d:%02d", rtc.getHours(),rtc.getMinutes(),rtc.getSeconds());

    String timestamp = strcat(dt, tm);
    Serial.println(timestamp);
    doc["DeviceTimestamp"]   = timestamp;
    serializeJson(doc, buffer);

    Serial.println(buffer);
    mqtt_client->publish(topic.c_str(), buffer);
    lastTelemetryMillis = millis();
  }

  // send a property update every 15 seconds
  if (mqtt_client->connected() && millis() - lastPropertyMillis > PROPERTY_SEND_INTERVAL) {
    Serial.println(F("Sending digital twin property ..."));

    String topic = (String)IOT_TWIN_REPORTED_PROPERTY;
    char buff[20];
    topic.replace(F("{request_id}"), itoa(requestId, buff, 10));
    String payload = F("{\"dieNumber\": {dieNumberValue}}");
    payload.replace(F("{dieNumberValue}"), itoa(dieNumberValue, buff, 10));

    mqtt_client->publish(topic.c_str(), payload.c_str());
    requestId++;

    lastPropertyMillis = millis();
  }
}

// Rohdaten umrechnen in Temperatur
//*************************************************************
// void getTemperature(int Rohwerte) {
//   // Formel der Standard AD-Wandlung + der Formel des AD8495
//   temperature = ((( Rohwerte * (AREF / (pow(2, ADC_RESOLUTION) - 1))) - 1.25) / 0.005);
//   // Anzeigen der Daten im Serial Monitor
//   Serial.print("Temperature (Typ K) = ");
//   Serial.print(temperature);
//   Serial.println(" C");
// }

// Sensordaten auslesen der BME - Sensoren
//*************************************************************
// void getBME(Adafruit_BME280 bme){
  
//   // Auslesen der BME Sensordaten 
// 	Serial.print("Temperature = ");
// 	Serial.print(bme.readTemperature());
// 	Serial.println("*C");

// 	Serial.print("Pressure = ");
// 	Serial.print(bme.readPressure() / 100.0F);
// 	Serial.println("hPa");

// 	Serial.print("Approx. Altitude = ");
// 	Serial.print(bme.readAltitude(SEALEVELPRESSURE_HPA));
// 	Serial.println("m");

// 	Serial.print("Humidity = ");
// 	Serial.print(bme.readHumidity());
// 	Serial.println("%");

// 	Serial.println();
// }
