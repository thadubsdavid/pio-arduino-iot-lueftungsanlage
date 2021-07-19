// Azure IoT Central device information
static char PROGMEM iotc_scopeId[] = "IoTHub-Lueftungsanlage.azure-devices.net";
//static char PROGMEM iotc_scopeId[] = "0ne0032003C";
static char PROGMEM iotc_deviceId[] = "TestDigitalLab";
static char PROGMEM iotc_deviceKey[] = "jeVGmbCzxCxNRgk5k3AeWPHJdoHDy1Hh3TUYDqHb/BU8zr87LMRxJrFVy/IKq/urC8Fubt1SHpgKjtyTxRhoGA==";
// static char PROGMEM iotc_hostName[] = "IoTHub-Lueftungsanlage.azure-devices.net";

// comment (simulated) / un-comment (real sensor data) to switch between sensor mode or simulated numbers
//#define READSENSORS

// Wi-Fi information
#if defined READSENSORS
static char PROGMEM wifi_ssid[] = "gpm-dtg-03";
static char PROGMEM wifi_password[] = "gpm-dtg-2020";
#else
static char PROGMEM wifi_ssid[] = "Ardu";
static char PROGMEM wifi_password[] = "12345678";
#endif

#define ARDUINO_SAMD_MKRWIFI1010

// Einstellungen Thermoelement
//*************************************************************
#define TC_PIN A0          // set to ADC pin used
#define AREF 3.3           // set to AREF, typically board voltage like 3.3 or 5.0
#define ADC_RESOLUTION 10  // set to ADC bit resolution, 10 is default

// Einstellungen BME280
//*************************************************************
#define SEALEVELPRESSURE_HPA (1013.25)
