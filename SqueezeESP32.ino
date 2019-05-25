#include <Arduino.h>
#include "config.h"
#include <WiFiUdp.h>
#include "slimproto.h"
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include "SSD1306Wire.h"
#include "OLEDDisplayUi.h"
#include "Wire.h"
#include "WeatherStationFonts.h"
#include <ArduinoJson.h>

const int I2C_DISPLAY_ADDRESS = 0x3c;
#ifdef ESP32
    #include <WiFi.h>
    #include <HTTPClient.h>
    #include <WebServer.h> 
    // Display Settings
    const int SDA_PIN = 4;
    const int SDC_PIN = 15;
#else
    #include <ESP8266WiFi.h>
    #include "ESP8266HTTPClient.h"
    #include "ESP8266httpUpdate.h"
    #include <ESP8266HTTPClient.h>
    #include <ESP8266WebServer.h>
    // Display Settings
    const int SDA_PIN = D1;
    const int SDC_PIN = D2;
#endif

#ifdef VS1053_MODULE
  #include "flac_plugin.h"
  #undef I2S_DAC_MODULE

  #ifdef ESP32
    #define VS1053_CS     5   // D1 // 5
    #define VS1053_DCS    17  // D0 // 16
    #define VS1053_DREQ   2   // D3 // 4
  #else // ESP8266
    #define VS1053_CS     D4 // was D1
    #define VS1053_DCS    D0 // 16
    #define VS1053_DREQ   D3 // 4
  #endif

  #ifdef ADAFRUIT_VS1053
    #include <Adafruit_VS1053.h>
    Adafruit_VS1053 viplayer(-1, VS1053_CS, VS1053_DCS, VS1053_DREQ);
  #else
    #include <VS1053.h>
    VS1053 viplayer(VS1053_CS, VS1053_DCS, VS1053_DREQ);
  #endif
#else
  #include "AudioFileSourceICYStream.h"
  #include "AudioFileSourceBuffer.h"
  #include "AudioGeneratorMP3.h"
  #include "AudioOutputI2SNoDAC.h"
#endif

// Initialize the oled display for address 0x3c
SSD1306Wire     display(I2C_DISPLAY_ADDRESS, SDA_PIN, SDC_PIN);

slimproto       *vislimCli = 0;
WiFiClient      client;
int             viCnxAttempt;
WiFiUDP         udp;
long            lastRetry;
HTTPClient      http;
DynamicJsonDocument root(1024);


#ifdef VS1053_MODULE
void LoadPlugin(const uint16_t* plugin, uint16_t plugin_size) {
    int i = 0;
    while (i<plugin_size) {
        uint16_t addr, n, val;
        addr = plugin[i++];
        n = plugin[i++];
        if (n & 0x8000U) { /* RLE run, replicate n samples */
            n &= 0x7FFF;
            val = plugin[i++];
            while (n--) {
                #ifdef ADAFRUIT_VS1053
                    viplayer.sciWrite(addr, val);
                #else
                    viplayer.write_register(addr, val);
                #endif
            }
        }
        else { /* Copy run, copy n samples */
            while (n--) {
                val = plugin[i++];
                #ifdef ADAFRUIT_VS1053
                    viplayer.sciWrite(addr, val);
                #else
                    viplayer.write_register(addr, val);
                #endif
            }
        }
    }
}
#endif //VS1053_MODULE


void setup() {
    #ifdef ESP32
      pinMode(16,OUTPUT);
      digitalWrite(16, LOW); // set GPIO16 low to reset OLED
      delay(50);
      digitalWrite(16, HIGH);
    #endif

    // initialize display
    display.init();
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setContrast(80);
    
    viCnxAttempt = 0;
    WiFiManager wifiManager;
    wifiManager.autoConnect("SqueezeEsp");

    Serial.begin(115200);
    delay(1000);
    Serial.println("Connecting to WiFi");

    SPI.begin();
    #ifdef VS1053_MODULE
        viplayer.begin();
        #ifndef ADAFRUIT_VS1053
            viplayer.switchToMp3Mode();
            LoadPlugin(plugin,PLUGIN_SIZE);
            viplayer.setVolume(80);
        #else
            viplayer.begin();
            //viplayer.applyPatch(plugin,PLUGIN_SIZE);
            LoadPlugin(plugin,PLUGIN_SIZE);
            viplayer.setVolume(20,20);
        #endif
    #endif

    udp.begin(UDP_PORT);
    connectToLMS();
    display.clear();
}

const char *title;
const char *artist;
const char *oldTitle;
int progress;
int oldVol;
int oldBuf;
int oldProgress;

void loop() {
    if (vislimCli->vcPlayerStat != vislimCli->PlayStatus && (millis() - lastRetry) > 30000) {
      lastRetry = millis();

      #ifndef ESP32
        t_httpUpdate_return ret = ESPhttpUpdate.update("192.168.0.1", 80, "/esp8266/ota.php", "0.1");

        switch(ret) {
            case HTTP_UPDATE_FAILED:
                Serial.printf("HTTP_UPDATE_FAILD Error (%d): %s", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
                break;

            case HTTP_UPDATE_NO_UPDATES:
                Serial.println("HTTP_UPDATE_NO_UPDATES");
                break;

            case HTTP_UPDATE_OK:
                ESP.eraseConfig();
                Serial.println("HTTP_UPDATE_OK");
                break;
        }
      #endif
    }
    
    if (client.connected()) {
        if (!vislimCli->HandleMessages()) {
            connectToLMS();
        }
        vislimCli->HandleAudio();
    }
    yield();

    int newVol = int(vislimCli->newvolume);
    if (newVol != oldVol || (millis() - lastRetry) > 3000) {
      lastRetry = millis();
      http.begin("http://192.168.0.1:9000/jsonrpc.js");
      String req = "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"00:00:00:00:00:01\",[\"status\",\"-\",1,\"tags:ad\"]]}";
      int httpCode = http.POST(req);
      deserializeJson(root, http.getString());
      if (root["result"]["playlist_loop"][0]["title"] != "") {
        title = root["result"]["playlist_loop"][0]["title"];
        artist = root["result"]["playlist_loop"][0]["artist"];
        progress = (int)((lastRetry - vislimCli->StartTimeCurrentSong) / (10*(float)root["result"]["playlist_loop"][0]["duration"]));
      }
      http.end();
   
      int newBuf = int(vislimCli->vcRingBuffer->dataSize()/vislimCli->vcRingBuffer->getBufferSize()*100);
      if (newVol != oldVol || ((newBuf != oldBuf || title != oldTitle || progress != oldProgress) && (progress >= 0 && progress <= 100)) ) {
        oldVol = newVol;
        oldBuf = newBuf;
        oldTitle = title;
        oldProgress = progress;
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        
        display.clear();
        display.drawString(0, 0, artist);
        display.drawString(0, 12, title);
        drawProgress(&display, progress, "Pro", 26);
        drawProgress(&display, newVol, "Vol", 39);
        drawProgress(&display, newBuf, "Buf", 52);
        display.display();        
      }
    }
}

void connectToLMS() {
    if (viCnxAttempt == -1) {
        LMS_addr = IPAddress(0,0,0,0);
    }

    // If no LMS specified in config
    if (LMS_addr[0] == 0) {
        findLMS();
    }

    if (LMS_addr[0] != 0) { //if LMS IP address was found or specified in config
        Serial.print("Connecting to server @");
        Serial.print(LMS_addr);
        Serial.println("..."); 

        // DEBUG server 3484
        // Real server 3483

        viCnxAttempt++;

        if (!client.connect(LMS_addr, 3483)) {
            Serial.println("connection failed, pause and try connect...");

            viCnxAttempt++;
            if (viCnxAttempt > 30)
                viCnxAttempt = -1; // Will erase LMS addr in the next attempt

            delay(2000);
            return;
        }


        viCnxAttempt = 0;

        if (vislimCli) delete vislimCli,vislimCli = 0;

        #ifdef VS1053_MODULE
            vislimCli = new slimproto(LMS_addr.toString(), & client, &viplayer);
        #else
            vislimCli = new slimproto(LMS_addr.toString(), client);
        #endif

        Serial.println("Connection Ok, send hello to LMS");
        reponseHelo *  HeloRsp = new reponseHelo(&client);
        HeloRsp->sendResponse();
    } else {
        Serial.println("No LMS server found, try again in 10 seconds"); 
        delay(10000);
        connectToLMS();
    }
}

void findLMS() {
    Serial.println("Search for LMS server..."); 
    for(int nbSend = 0; nbSend < 10; nbSend++) {
        // start UDP server
        //Send udp packet for autodiscovery
        udp.flush();
        udp.beginPacket("255.255.255.255",UDP_PORT);
        udp.printf("e");
        udp.endPacket();

        delay(2000);

        if (udp.parsePacket()> 0) {
            char upd_packet; 
            upd_packet = udp.read();

            if (upd_packet == 'E') {
                LMS_addr = udp.remoteIP();
                Serial.print("Found LMS server @ "); 
                Serial.println(LMS_addr); 
                //udp.stop(); 
                break; // LMS found we can go to the next step
            }
        } else {
            delay(2000);
        }
    }
}

void drawProgress(OLEDDisplay *display, int percentage, String label, int y) {
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_10);
  display->drawString(7, y, label);
  display->drawProgressBar(19, y+1, 108, 10, percentage);
  display->display();
}

