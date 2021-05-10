/* This is some Arduino IDE code for an ESP32 4 channel relay. The relay used ON=Low  OFF=High 
 *  using ESP32 with Tasmota or ESPHome with the LOW=ON relay it was always turning on for a split second on boot. I wanted all relays always off on boot. 
 *  
 *  This code is setup for an ESP32 Wemos D1 mini
 */

#include "EspMQTTClient.h"
#include "ArduinoJson.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ArduinoQueue.h>

/****************** CONFIGURATIONS TO CHANGE *******************/

static const char* host = "esp32";                   //  hostname defaults is esp32
static const char* ssid = "SSID";                      //  WIFI SSID
static const char* password = "Password";    //  WIFI Password

static String otaUserId = "admin";                   //  user Id for OTA update
static String otaPass = "admin";                     //  password for OTA update
static WebServer server(80);                         //  default port 80

static std::string ESPMQTTTopic = "relayMQTT";       //  MQTT main topic

static EspMQTTClient client(
  ssid,
  password,
  "192.168.1.XXX",                            //  MQTT Broker server ip
  "MQTTUsername",                             //  Can be omitted if not needed
  "MQTTPassword",                             //  Can be omitted if not needed
  "ESPRELAYMQTT",                             //  Client name that uniquely identify your device
  1883                                        //  MQTT Port
);

static const char * relay1 = "relay1";
static const char * relay2 = "relay2";
static const char * relay3 = "relay3";
static const char * relay4 = "relay4";
static std::string mqttDeviceTopic = ESPMQTTTopic + "/relays/";

static const int switchPin1 = 16;
static const int switchPin2 = 17;
static const int switchPin3 = 18;
static const int switchPin4 = 19;

/*************************************************************/


/*
   Login page
*/

static const String versionNum = "1.0";
static const String loginIndex =
  "<form name='loginForm'>"
  "<table width='20%' bgcolor='A09F9F' align='center'>"
  "<tr>"
  "<td colspan=2>"
  "<center><font size=4><b>Relay ESP32 MQTT version: " + versionNum + "</b></font></center>"
  "<center><font size=2><b>(Unofficial)</b></font></center>"
  "<br>"
  "</td>"
  "<br>"
  "<br>"
  "</tr>"
  "<tr>"
  "<td>Username:</td>"
  "<td><input type='text' size=25 name='userid'><br></td>"
  "</tr>"
  "<br>"
  "<br>"
  "<tr>"
  "<td>Password:</td>"
  "<td><input type='Password' size=25 name='pwd'><br></td>"
  "<br>"
  "<br>"
  "</tr>"
  "<tr>"
  "<td><input type='submit' onclick='check(this.form)' value='Login'></td>"
  "</tr>"
  "</table>"
  "</form>"
  "<script>"
  "function check(form)"
  "{"
  "if(form.userid.value=='" + otaUserId + "' && form.pwd.value=='" + otaPass + "')"
  "{"
  "window.open('/serverIndex')"
  "}"
  "else"
  "{"
  " alert('Error Password or Username')/*displays error message*/"
  "}"
  "}"
  "</script>";

/*
   Server Index Page
*/

static const String serverIndex =
  "<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
  "<form method='POST' action='#' enctype='multipart/form-data' id='upload_form'>"
  "<input type='file' name='update'>"
  "<input type='submit' value='Update'>"
  "</form>"
  "<div id='prg'>progress: 0%</div>"
  "<script>"
  "$('form').submit(function(e){"
  "e.preventDefault();"
  "var form = $('#upload_form')[0];"
  "var data = new FormData(form);"
  " $.ajax({"
  "url: '/update',"
  "type: 'POST',"
  "data: data,"
  "contentType: false,"
  "processData:false,"
  "xhr: function() {"
  "var xhr = new window.XMLHttpRequest();"
  "xhr.upload.addEventListener('progress', function(evt) {"
  "if (evt.lengthComputable) {"
  "var per = evt.loaded / evt.total;"
  "$('#prg').html('progress: ' + Math.round(per*100) + '%');"
  "}"
  "}, false);"
  "return xhr;"
  "},"
  "success:function(d, s) {"
  "console.log('success!')"
  "},"
  "error: function (a, b, c) {"
  "}"
  "});"
  "});"
  "</script>";

static std::string esp32Str = ESPMQTTTopic + "/ESP32";
static std::string lastWillStr = ESPMQTTTopic + "/lastwill";
static const char* lastWill = lastWillStr.c_str();
static std::string controlStdStr = ESPMQTTTopic + "/control";
static bool firstBoot = true;

static const String controlStr = controlStdStr.c_str();

struct to_lower {
  int operator() ( int ch )
  {
    return std::tolower ( ch );
  }
};

struct QueueCommand {
  std::string payload;
  std::string topic;
};

static int queueSize = 50;
ArduinoQueue<QueueCommand> commandQueue(queueSize);

void setup () {
  pinMode(switchPin1, OUTPUT); // Relay Switch 1
  digitalWrite(switchPin1, HIGH);
  pinMode(switchPin2, OUTPUT); // Relay Switch 2
  digitalWrite(switchPin2, HIGH);
  pinMode(switchPin3, OUTPUT); // Relay Switch 3
  digitalWrite(switchPin3, HIGH);
  pinMode(switchPin4, OUTPUT); // Relay Switch 4
  digitalWrite(switchPin4, HIGH);

  // Connect to WiFi network
  WiFi.begin(ssid, password);
  Serial.println("");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  /*use mdns for host name resolution*/
  if (!MDNS.begin(host)) { //http://esp32.local
    Serial.println("Error setting up MDNS responder!");
    while (1) {
      delay(1000);
    }
  }
  Serial.println("mDNS responder started");
  /*return index page which is stored in serverIndex */
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", loginIndex);
  });
  server.on("/serverIndex", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", serverIndex);
  });
  /*handling uploading firmware file */
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      /* flashing firmware to ESP*/
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });
  server.begin();
  client.setMqttReconnectionAttemptDelay(100);
  client.enableLastWillMessage(lastWill, "Offline");
  client.setKeepAlive(60);
  Serial.begin(115200);
}

void loop () {
  server.handleClient();
  client.loop();
  processQueue();
  updateStatuses();
  delay(1);
}

static long lastRescan = 0;
static int rescanTime = 30;
void updateStatuses() {
  if (!firstBoot) {
    if ((millis() - lastRescan) >= (rescanTime * 1000)) {
      int state1 = digitalRead(switchPin1);
      int state2 = digitalRead(switchPin2);
      int state3 = digitalRead(switchPin3);
      int state4 = digitalRead(switchPin4);

      if (state1 == LOW) {
        client.publish((mqttDeviceTopic + relay1).c_str(), "{\"status\":\"ON\"}");
      }
      else if (state1 == HIGH) {
        client.publish((mqttDeviceTopic + relay1).c_str(), "{\"status\":\"OFF\"}");
      }

      if (state2 == LOW) {
        client.publish((mqttDeviceTopic + relay2).c_str(), "{\"status\":\"ON\"}");
      }
      else if (state2 == HIGH) {
        client.publish((mqttDeviceTopic + relay2).c_str(), "{\"status\":\"OFF\"}");
      }

      if (state3 == LOW) {
        client.publish((mqttDeviceTopic + relay3).c_str(), "{\"status\":\"ON\"}");
      }
      else if (state3 == HIGH) {
        client.publish((mqttDeviceTopic + relay3).c_str(), "{\"status\":\"OFF\"}");
      }

      if (state4 == LOW) {
        client.publish((mqttDeviceTopic + relay4).c_str(), "{\"status\":\"ON\"}");
      }
      else if (state4 == HIGH) {
        client.publish((mqttDeviceTopic + relay4).c_str(), "{\"status\":\"OFF\"}");
      }
      lastRescan = millis();
    }
  }
}

bool processQueue() {
  struct QueueCommand aCommand;
  while (!commandQueue.isEmpty()) {
    aCommand = commandQueue.getHead();
    if (aCommand.topic == ESPMQTTTopic + "/control") {
      controlMQTT(aCommand.payload);
    }
    commandQueue.dequeue();
  }
  return true;
}

bool is_number(const std::string & s)
{
  std::string::const_iterator it = s.begin();
  while (it != s.end() && std::isdigit(*it)) ++it;
  return !s.empty() && it == s.end();
}

void controlMQTT(std::string payload) {
  Serial.println("Processing Control MQTT...");
  StaticJsonDocument<100> docIn;
  deserializeJson(docIn, payload);
  if (docIn == nullptr) { //Check for errors in parsing
    char aBuffer[100];
    StaticJsonDocument<100> docOut;
    Serial.println("Parsing failed");
    docOut["status"] = "errorParsingJSON";
    serializeJson(docOut, aBuffer);
    client.publish(esp32Str.c_str(), aBuffer);
  }
  else {
    const char * aName = docIn["id"];
    const char * value = docIn["value"];
    std::string deviceStr = mqttDeviceTopic + aName;
    std::string aName2 = aName;
    std::string value2 = value;
    bool isNum = is_number(aName);
    bool isNum2 = is_number(value);
    int aVal = 99;
    int aVal2 = 99;
    if (isNum ) {
      sscanf(aName, "%d", &aVal);
    }
    if (isNum2) {
      sscanf(value, "%d", &aVal2);
    }
    std::transform(aName2.begin(), aName2.end(), aName2.begin(), to_lower());
    std::transform(value2.begin(), value2.end(), value2.begin(), to_lower());
    aName = aName2.c_str();
    value = value2.c_str();
    if (aVal == 1 || (strcmp(aName, relay1) == 0)) {
      if (aVal2 == 1 || (strcmp(value, "off") == 0) ) {
        digitalWrite(switchPin1, HIGH);
        client.publish(deviceStr.c_str(), "{\"status\":\"OFF\"}");
      }
      else if (aVal2 == 0 || (strcmp(value, "on") == 0)) {
        digitalWrite(switchPin1, LOW);
        client.publish(deviceStr.c_str(), "{\"status\":\"ON\"}");
      }
    }
    else if (aVal == 2 || (strcmp(aName, relay2) == 0)) {
      if (aVal2 == 1 || (strcmp(value, "off") == 0) ) {
        digitalWrite(switchPin2, HIGH);
        client.publish(deviceStr.c_str(), "{\"status\":\"OFF\"}");
      }
      else if (aVal2 == 0 || (strcmp(value, "on") == 0)) {
        digitalWrite(switchPin2, LOW);
        client.publish(deviceStr.c_str(), "{\"status\":\"ON\"}");
      }
    }
    else if (aVal == 3 || (strcmp(aName, relay3) == 0)) {
      if (aVal2 == 1 || (strcmp(value, "off") == 0) ) {
        digitalWrite(switchPin3, HIGH);
        client.publish(deviceStr.c_str(), "{\"status\":\"OFF\"}");
      }
      else if (aVal2 == 0 || (strcmp(value, "on") == 0)) {
        digitalWrite(switchPin3, LOW);
        client.publish(deviceStr.c_str(), "{\"status\":\"ON\"}");
      }
    }
    else if (aVal == 4 || (strcmp(aName, relay4) == 0)) {
      if (aVal2 == 1 || (strcmp(value, "off") == 0) ) {
        digitalWrite(switchPin4, HIGH);
        client.publish(deviceStr.c_str(), "{\"status\":\"OFF\"}");
      }
      else if (aVal2 == 0 || (strcmp(value, "on") == 0)) {
        digitalWrite(switchPin4, LOW);
        client.publish(deviceStr.c_str(), "{\"status\":\"ON\"}");
      }
    }
  }

  delay(100);
  client.publish(esp32Str.c_str(), "{\"status\":\"idle\"}");
}

void onConnectionEstablished() {
  if (firstBoot) {
    firstBoot = false;
    client.publish((mqttDeviceTopic + relay1).c_str(), "{\"status\":\"OFF\"}");
    client.publish((mqttDeviceTopic + relay2).c_str(), "{\"status\":\"OFF\"}");
    client.publish((mqttDeviceTopic + relay3).c_str(), "{\"status\":\"OFF\"}");
    client.publish((mqttDeviceTopic + relay4).c_str(), "{\"status\":\"OFF\"}");
    lastRescan = millis();
  }
  client.subscribe(controlStr, [] (const String & payload)  {
    Serial.println("Control MQTT Received...");
    if (!commandQueue.isFull()) {
      struct QueueCommand queueCommand;
      queueCommand.payload = payload.c_str();
      queueCommand.topic = ESPMQTTTopic + "/control";
      commandQueue.enqueue(queueCommand);
    }
    else {
      client.publish(esp32Str.c_str(), "{\"status\":\"errorQueueFull\"}");
    }
  });

}
