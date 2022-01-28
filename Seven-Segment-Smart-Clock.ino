#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SevSegShift.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPUpdateServer.h>
#include <OneWire.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Timezone.h>

#define DEBUG 0
#if DEBUG == 1
#define debug(x) Serial.print(x)
#define debugln(x) Serial.println(x)
#define serial(x) Serial.begin(x)
#else
#define debug(x)
#define debugln(x)
#define serial(x)
#endif

#define SHIFT_PIN_DS   D0 /* Data input PIN */
#define SHIFT_PIN_STCP D1 /* Shift Register Storage PIN */
#define SHIFT_PIN_SHCP D2 /* Shift Register Shift PIN */

#define ONE_WIRE_BUS D3

OneWire oneWire(ONE_WIRE_BUS);

struct Config {
  char WIFI_SSID[33];
  char WIFI_KEY[64];
  String HOSTNAME;
  float TEMP_OFFSET;
};

String version = "1.0.1";
float temp;
int last_min, last_hour = -1;
char last_time[] = "000.0 000.";
unsigned long last_temp_time = 0;

byte oneWire_data = D3;
int face_status = 1;
int brightness = 100;

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater; 

const char *filename_conf = "/config.json";
Config config;

SevSegShift sevseg(SHIFT_PIN_DS, SHIFT_PIN_SHCP, SHIFT_PIN_STCP);

byte segmentPins[] = {5,2,7,1,3,6,0,4}; // ABCDEFGP
byte digitPins[] = {13,9,15,14,11,10,12,8}; 

// NTP
WiFiUDP ntpUDP;

int GTMOffset = 0;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", GTMOffset*60*60, 60*60*1000);

TimeChangeRule CEST = {"CEST", Last, Sun, Mar, 2, 120};     // Central European Summer Time
TimeChangeRule CET = {"CET ", Last, Sun, Oct, 3, 60};       // Central European Standard Time
Timezone CE(CEST, CET);

void loadConfiguration(const char *filename_conf, Config &config) {
  File file = LittleFS.open(filename_conf, "r");
  StaticJsonDocument<256> doc;

  DeserializationError error = deserializeJson(doc, file);
  if (error){
    debugln(F("Failed to read file, using default configuration"));
  }

  strlcpy(config.WIFI_SSID, doc["WIFI_SSID"] | "Your WIFI-SSID", sizeof(config.WIFI_SSID));
  strlcpy(config.WIFI_KEY, doc["WIFI_KEY"] | "xxxxxxxx", sizeof(config.WIFI_KEY));
  config.TEMP_OFFSET = doc["TEMP_OFFSET"] | float(0);
  config.HOSTNAME = doc["HOSTNAME"] | String("ESP8266-Clock");

  debugln("WIFI_SSID: " + String(config.WIFI_SSID));
  debugln("KEY: " + String(config.WIFI_KEY));
  debugln("TEMP_OFFSET: " + String(config.TEMP_OFFSET,3));
  debugln("HOSTNAME: " + config.HOSTNAME);

  file.close();
}

void saveConfiguration(const char *filename_conf, const Config &config) {
  LittleFS.remove(filename_conf);

  File file = LittleFS.open(filename_conf, "w");
  if (!file) {
    debugln("Failed to create file");
    return;
  }

  StaticJsonDocument<256> doc;

  doc["WIFI_SSID"] = config.WIFI_SSID;
  doc["WIFI_KEY"] = config.WIFI_KEY;
  doc["TEMP_OFFSET"] = config.TEMP_OFFSET;
  doc["HOSTNAME"] = config.HOSTNAME;

  if (serializeJson(doc, file) == 0) {
    debugln("Failed to write to file");
  }
  file.close();
}

void(* resetFunc) (void) = 0;

void setup() {
  serial(115200);
  debugln("Begin setup");
  byte numDigits = 8;
  bool resistorsOnSegments = false; // 'false' means resistors are on digit pins
  byte hardwareConfig = COMMON_ANODE; // See README.md for options
  bool updateWithDelays = false; // Default 'false' is Recommended
  bool leadingZeros = false; // Use 'true' if you'd like to keep the leading zeros
  bool disableDecPoint = false; // Use 'true' if your decimal point doesn't exist or isn't connected. Then, you only need to specify 7 segmentPins[]

  sevseg.begin(hardwareConfig, numDigits, digitPins, segmentPins, resistorsOnSegments, updateWithDelays, leadingZeros, disableDecPoint);

  if (!LittleFS.begin()){
    debugln("An Error has occurred while mounting SPIFFS");
    return;
  }

  debugln("Loading configuration...");
  loadConfiguration(filename_conf, config);

  WiFi.hostname(config.HOSTNAME);
  WiFi.begin(config.WIFI_SSID, config.WIFI_KEY);  //Connect to the WiFi network

  int nr = 0;
  debug("Conecting to Wifi");
  while (WiFi.status() != WL_CONNECTED) {  //Wait for connection
    delay(500);
    debug(".");
    nr++;
    if (nr > 60) {
      debugln("\nStart recovery_ap");
      WiFi.disconnect();
      WiFi.softAP("ESP8266-Clock-Setup", "12345678");
      debugln("SSID: "+ String(WiFi.softAPSSID()));
      debugln("PW: "+ String(WiFi.softAPPSK()));
      debugln("IP: "+ WiFi.softAPIP().toString());
      sevseg.setNumber(11111111);
      face_status = 2;
      break;
    }
  }
    
  if (WiFi.status() == WL_CONNECTED) { debugln("\nIP address: " + WiFi.localIP().toString());}
  timeClient.begin();
  delay ( 1000 );
  if (timeClient.update()){
   debug( "Adjust local clock to: ");
   unsigned long epoch = timeClient.getEpochTime();
   setTime(CE.toLocal(epoch));
   debugln(String(hour()) + ":" + String(minute()) + ":" + String(second()));
  }else{
    debugln( "NTP Update not WORK!!" );
  }

  oneWire.reset();
  oneWire.skip();
  oneWire.write(0x44, 0);
  httpUpdater.setup(&server);
  server.on("/", configurehandler);
  server.on("/setconfig", handleconfig);
  server.on("/gettemp", handletemp);
  server.begin();
  debugln("Setup complete\n");
}

void handleconfig() {
  String tmp;
  if(server.hasArg("SSID") && server.arg("SSID") != "" && server.arg("SSID") != config.WIFI_SSID){
    tmp = server.arg("SSID");
    debug("New SSID: ");
    debugln(tmp);
    strlcpy(config.WIFI_SSID, tmp.c_str(), sizeof(config.WIFI_SSID));
  }
  if(server.hasArg("PW") && server.arg("PW") != "" && server.arg("PW") != config.WIFI_KEY){
    tmp = server.arg("PW");
    debug("New PW: ");
    debugln(tmp);
    strlcpy(config.WIFI_KEY, tmp.c_str(), sizeof(config.WIFI_KEY));
  }
  if(server.hasArg("TEMP_OFFSET") && server.arg("TEMP_OFFSET") != "" && server.arg("TEMP_OFFSET").toFloat() != config.TEMP_OFFSET){
    tmp = server.arg("TEMP_OFFSET");
    tmp.replace(",",".");
    float tmsp = tmp.toFloat();
    debug("New TEMP_OFFSET: ");
    debugln(tmp);
    config.TEMP_OFFSET = tmsp;
  }
  if(server.hasArg("HOSTNAME") && server.arg("HOSTNAME") != "" && server.arg("HOSTNAME") != config.HOSTNAME){
    tmp = server.arg("HOSTNAME");
    debug("New HOSTNAME: ");
    debugln(tmp);
    config.HOSTNAME = tmp;
  }
  if ((server.arg("SSID") != "" || server.arg("PW") != "" || server.arg("HOSTNAME") != "")){
    debugln("Reset");
    server.send(200, "text/html", "<html><head><title>ESP8266-Clock Web-UI</title><meta http-equiv='refresh' content='5; url=/'><style>#text,h2{text-align:center}body{font-family:Helvetica;background-color:#161618;color:#f2f5f4}</style></head><body><h2>ESP8266-Clock Web-UI</h2><div id='text'><p>Configuration updated!</p><p>Restarting!</p></div></body></html>");
    saveConfiguration(filename_conf, config);
    delay(3000);
    resetFunc();
  } else {
    server.send(200, "text/html", "<html><head><title>ESP8266-Clock Web-UI</title><meta http-equiv='refresh' content='5; url=/'><style>#text,h2{text-align:center}body{font-family:Helvetica;background-color:#161618;color:#f2f5f4}</style></head><body><h2>ESP8266-Clock Web-UI</h2><div id='text'><p>Configuration updated!</p></div></body></html>");
    saveConfiguration(filename_conf, config);
  }
}

void handletemp(){
  server.send(200, "text/plain", String(get_temp(),3));
}
 
void configurehandler() {
  server.send(200, "text/html", "<html><head><title>ESP8266-Clock Web-UI</title><style>#formdiv,h2{text-align:center}label{display:block;margin-right:auto;margin-left:auto}#upfirm{position:fixed;color:#888;bottom:8px;right:16px}#ver{position:fixed;color:#888;bottom:8px;left:16px}body{font-family:Helvetica;background-color:#161618;color:#f2f5f4}</style></head><body><h2>ESP8266-Clock Web-UI</h2><div id='formdiv'><form action='/setconfig' onsubmit='return confirm(\"Save settings?\")'><label for='fname'>WIFI Name ("+String(config.WIFI_SSID)+"):</label><input id='SSID' name='SSID'><br><br><label for='lname'>WIFI PW:</label><input type='password' id='PW' name='PW'><br><br><label for='lname'>TEMP OFFSET ("+String(config.TEMP_OFFSET,3)+"):</label><input id='TEMP_OFFSET' name='TEMP_OFFSET'><br><br><label for='lname'>Hostname ("+(config.HOSTNAME)+"):</label><input id='HOSTNAME' name='HOSTNAME'><br><br><input type='submit' value='Submit'></form></div><a id='upfirm' href='/update'>Update Firmware</a> <a id='ver'>"+version+"</a></body></html>");
}


float rounding(float in, byte decimalplace) {
  float shift = 1;
  while (decimalplace--) {
    shift *= 10;
  }
  return floor(in * shift + .5) / shift;
}

float get_temp_refresh() {
  oneWire.reset();
  sevseg.refreshDisplay();
  oneWire.skip();
  sevseg.refreshDisplay();
  oneWire.write(0xBE);
  sevseg.refreshDisplay();

  uint8_t data[9];
  for (uint8_t i = 0; i < 9; i++) {
    sevseg.refreshDisplay();
    data[i] = oneWire.read();
    sevseg.refreshDisplay();
  }
  int16_t rawTemperature = (((int16_t)data[1]) << 8) | data[0];
  sevseg.refreshDisplay();
  float temp = 0.0625 * rawTemperature;
  sevseg.refreshDisplay();
  oneWire.reset();
  sevseg.refreshDisplay();
  oneWire.skip();
  sevseg.refreshDisplay();
  oneWire.write(0x44, 0);
  sevseg.refreshDisplay();
  return temp;
}

float get_temp() {
  oneWire.reset();
  oneWire.skip();
  oneWire.write(0xBE);

  uint8_t data[9];
  for (uint8_t i = 0; i < 9; i++) {
    data[i] = oneWire.read();
  }
  int16_t rawTemperature = (((int16_t)data[1]) << 8) | data[0];
  float temp = 0.0625 * rawTemperature;
  oneWire.reset();
  oneWire.skip();
  oneWire.write(0x44, 0);
  return temp;
}

void faceClock(){
    if (last_hour != hour()) {
    setTime(CE.toLocal(timeClient.getEpochTime()));
    if (hour() <= 9) {
      last_time[0] = '0';
      last_time[1] = 48 + hour();
    } else {

      last_time[0] = 48 + ((hour()/10)%10);
      last_time[1] = 48 + (hour()%10);
    }
    sevseg.setChars(last_time);
    last_hour = hour();
  }
  
  if (last_min != minute()) {
    if (minute() <= 9) {
      last_time[2] = '0';
      last_time[4] = 48 + minute();
    } else {
      last_time[2] = 48 + ((minute()/10)%10);
      last_time[4] = 48 + (minute()%10);
    }
    sevseg.setChars(last_time);
    last_min = minute();
  }

  if (millis() - last_temp_time >= 20000) {
    float a;
    if (face_status) {
      a = (rounding((get_temp_refresh() + config.TEMP_OFFSET), 1)*10);
    }
    else {
      a = (rounding((get_temp() + config.TEMP_OFFSET), 1)*10);
    }
    if (int(a) >= 100) {
      last_time[6] = 48 + (int(a)/100) % 10;
    } else {
      last_time[6] = last_time[5];
    }
    last_time[7] = 48 + (int(a)/10) % 10;
    last_time[8] = 48 + (int(a) % 10);
    last_temp_time = millis();
    sevseg.setChars(last_time);
  }
}

void loop() {
  server.handleClient();
  if (face_status == 1) {
    faceClock();
    sevseg.refreshDisplay();
  }
}
