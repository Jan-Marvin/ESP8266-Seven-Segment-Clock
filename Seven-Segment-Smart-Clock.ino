#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SevSegShift.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPUpdateServer.h>
#include <OneWire.h>
#include <ezTime.h>

#define DEBUG 0
#if DEBUG == 1
#define debug(x) Serial.print(x)
#define debugln(x) Serial.println(x)
#define serial(x) Serial.begin(x)
#define debugstat "-d"
#else
#define debug(x)
#define debugln(x)
#define serial(x)
#define debugstat 
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

String version = "1.0.5";
String led_stat = "checked";
float temp;
char display[] = "000.0 000.";

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

Timezone Germany;

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

void setup() {
  serial(115200);
    while (!Serial) {};
  debugln("###Begin setup###");
  
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
    
  debugln("\nIP address: " + WiFi.localIP().toString());
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  delay ( 1000 );

  debugln("Get & set time");
  waitForSync();
  Germany.setLocation("Europe/Berlin");
  Serial.println("Time: " + Germany.dateTime());

  display[0] = Germany.dateTime("H").charAt(0);
  display[1] = Germany.dateTime("H").charAt(1);
  display[2] = Germany.dateTime("i").charAt(0);
  display[4] = Germany.dateTime("i").charAt(1);
  
  debugln("Get & Set Temp");
  float a = (rounding((get_temp() + config.TEMP_OFFSET), 1)*10);
  if (int(a) >= 100) {
    display[6] = 48 + (int(a)/100) % 10;
  } else {
    display[6] = display[5];
  }
  display[7] = 48 + (int(a)/10) % 10;
  display[8] = 48 + (int(a) % 10);
  
  httpUpdater.setup(&server);
  server.on("/", configurehandler);
  server.on("/setconfig", handleconfig);
  server.on("/gettemp", handletemp);
  server.begin();
  debugln("###Setup complete###\n");
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
  if(server.hasArg("BRIGHTNESS") && server.arg("BRIGHTNESS") != "" && server.arg("BRIGHTNESS") != String(brightness)){
    tmp = server.arg("BRIGHTNESS");
    int tmsp = tmp.toInt();
    if (tmsp > 200) {
      tmsp = 200;
    } else if (tmsp < -200) {
      tmsp = -200;
    }
    debug("New BRIGHTNESS: ");
    debugln(tmsp);
    brightness = tmsp;
    sevseg.setBrightness(brightness);
  }
  
  if(server.hasArg("LED")){
    tmp = server.arg("LED");
      if (led_stat == "") {
        led_stat = "checked";
        face_status = 1;
        debugln("New LED stat: on");
      }
    } else {
      if (led_stat == "checked") {
        led_stat = "";
        face_status = 0;
        sevseg.blank();
        debugln("New LED stat: off");
      }    
  }
  if ((server.arg("SSID") != "" || server.arg("PW") != "" || server.arg("HOSTNAME") != "")){
    debugln("Reset");
    server.send(200, "text/html", "<html><head><title>ESP8266-Clock Web-UI</title><meta http-equiv='refresh' content='5; url=/'><style>#text,h2{text-align:center}body{font-family:Helvetica;background-color:#161618;color:#f2f5f4}</style></head><body><h2>ESP8266-Clock Web-UI</h2><div id='text'><p>Configuration updated!</p><p>Restarting, pleas wait!</p></div></body></html>");
    saveConfiguration(filename_conf, config);
    delay(3000);
    resetFunc();
  } else {
    server.send(200, "text/html", "<html><head><title>ESP8266-Clock Web-UI</title><meta http-equiv='refresh' content='3; url=/'><style>#text,h2{text-align:center}body{font-family:Helvetica;background-color:#161618;color:#f2f5f4}</style></head><body><h2>ESP8266-Clock Web-UI</h2><div id='text'><p>Configuration updated, pleas wait!</p></div></body></html>");
    saveConfiguration(filename_conf, config);
  }
}

void handletemp(){
  server.send(200, "text/plain", String(get_temp(),3));
}
 
void configurehandler() {
  server.send(200, "text/html", "<html><head><title>ESP8266-Clock Web-UI</title><style>#formdiv,h2{text-align:center}label{display:block;margin-right:auto;margin-left:auto}#upfirm{position:fixed;color:#888;bottom:8px;right:16px}#ver{position:fixed;color:#888;bottom:8px;left:16px}body{font-family:Helvetica;background-color:#161618;color:#f2f5f4}</style></head><body><h2>ESP8266-Clock Web-UI</h2><div id='formdiv'><form action='/setconfig' onsubmit='return confirm(\"Save settings?\")'><label for='fname'>WIFI Name ("+String(config.WIFI_SSID)+"):</label><input id='SSID' name='SSID'><br><br><label for='lname'>WIFI PW:</label><input type='password' id='PW' name='PW'><br><br><label for='lname'>TEMP OFFSET ("+String(config.TEMP_OFFSET,3)+"):</label><input id='TEMP_OFFSET' name='TEMP_OFFSET'><br><br><label for='lname'>Hostname ("+(config.HOSTNAME)+"):</label><input id='HOSTNAME' name='HOSTNAME'><br><br><label for='lname'>Brightness ("+String(brightness)+"):</label><input id='BRIGHTNESS' name='BRIGHTNESS'><br><br><label1 for='lname'>LED status:</label1><input type='checkbox' id='LED' name='LED'"+ led_stat+"><br><br><input type='submit' value='Submit'></form></div><a id='upfirm' href='/update'>Update Firmware</a> <a id='ver'>"+ version + debugstat +"</a></body></html>");
}


float rounding(float in, byte decimalplace) {
  float shift = 1;
  while (decimalplace--) {
    shift *= 10;
  }
  return floor(in * shift + .5) / shift;
}

void faceClock(){
  if (minuteChanged()) {
    float a;
    if (face_status == 1) {
      a = (rounding((get_temp_refresh() + config.TEMP_OFFSET), 1)*10);
    }
    else {
      a = (rounding((get_temp() + config.TEMP_OFFSET), 1)*10);
    }
    if (int(a) >= 100) {
      display[6] = 48 + (int(a)/100) % 10;
    } else {
      display[6] = display[5];
    }
    display[7] = 48 + (int(a)/10) % 10;
    display[8] = 48 + (int(a) % 10);
    display[0] = Germany.dateTime("H").charAt(0);
    display[1] = Germany.dateTime("H").charAt(1);
    display[2] = Germany.dateTime("i").charAt(0);
    display[4] = Germany.dateTime("i").charAt(1);
  }
  sevseg.setChars(display);
}

void loop() {
  events();
  server.handleClient();
  faceClock();
  if (face_status == 1){
    sevseg.refreshDisplay();
  }
}
