#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h> 
#include <WiFiManager.h> 
#include <ArduinoOTA.h>
#include <WebSocketsClient.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <Ticker.h>
#include <RCSwitch.h>
#include <OneWire.h>
#include "DHT.h"
#include <DallasTemperature.h>

const byte GS_pin=D4;
const byte RS_pin=D3;
const byte CS_pin=D8;
const byte MISO_pin=D6;
const byte SCLK_pin=D5;



int DS18B20_Pin=D7;
int T1=0, T2=0;
uint8_t Reconnect_MQTT_Attempts=0, Reconnect_WIFI_Attempts=0;

unsigned long Timer1=0,lastReconnectAttempt_MQTT=0,lastReconnectAttempt_WIFI=0,timeSinceLastRead=0, SinceLastHeartbeat=0;
int datat;
const char *mqtt_broker = "broker.hivemq.com";
const char *topic = "pastrevys/namai/virtuve";
const char *mqtt_username = "karolis";
const char *mqtt_password = "223";
const int   mqtt_port = 1883;

uint8_t Virtuves_temp=0, Dumtraukio_temp=0;

class High_Temp_measure {

  private:

  uint8_t MISO_PIN, SCLK_PIN, CS_PIN;
  uint16_t LastTemp=0;
  
  double read() {
    digitalWrite(this->CS_PIN, LOW);      // lustas aktyvuojamas skaitymui
    delay(1);
    uint16_t val2 = spiread();
    val2 <<= 8;                           // nuskaityta 1 baita perstumiam per 8 vietas
    val2 |= spiread();                    // skaitomas 2 baitas
    digitalWrite(this->CS_PIN, HIGH);     // lusto aktyvumas isjungiamas
    if (val2 & 0x4) {                     // jei 100 ty 2 bitas 1 kazkas blogai su davikliu
      return NAN;
    } 
    val2 >>= 3;                           // temperatura perduodama 15-3 bitais 15 visada 0 paskutiniai 3 nebeaktualus. 
    return (val2 * 0.25);
}

  byte spiread() {  //Nuskaitoma po 1 baita 
    byte val = 0;
    for (int i = 7; i >= 0; --i) {  
      digitalWrite(this->SCLK_PIN, LOW);
      delayMicroseconds(10);              //clock low cycle

      if (digitalRead(this->MISO_PIN)) {
        val |= (1 << i);                  // nuskaitomas bitas ir perstumiamas per i
      }
      digitalWrite(this->SCLK_PIN, HIGH); // clock high cycle
      delayMicroseconds(10);

    }

  return val;
}


  public:

  High_Temp_measure (uint8_t MISO_PIN, uint8_t SCLK_PIN, uint8_t CS_PIN)  //Pradiniai parametrai
  {
    pinMode(this->SCLK_PIN = SCLK_PIN, OUTPUT);
    pinMode(this->CS_PIN = CS_PIN, OUTPUT);
    pinMode(this->MISO_PIN = MISO_PIN, INPUT);
    digitalWrite(this->CS_PIN, HIGH);
  }


  int ReadTemp (){
    Dumtraukio_temp=(int)read();
    return Dumtraukio_temp;
  }

};

class Motor {

  private:
    uint8_t pin;
    bool ON;
    unsigned long runtime =0;
    double kwh;
    String title;
    bool workingMode=false;  // 0=manual, 1=AUTO
    unsigned long lastSwitchTime=0; //Pagal gamintojo rekomendacijas siurblys negali isijungti dazniau nei 2 kartus per valanda
    unsigned long WaitTimeBetweenSwitching=30000; // 30min

    int runtimeCalc(){
      runtime +=lastSwitchTime/60000; // grazina veikimo laika min
      kwh+=runtime/60*0.015;
    }

    bool ableToSwitch (){
      if (millis()-lastSwitchTime>WaitTimeBetweenSwitching) return 1;
      else return 0;
    }

  public:
    Motor (byte Pin, String Title) {
      pin=Pin;
      title=Title;
    }



    void init () {
      pinMode(pin, OUTPUT);
      digitalWrite(pin,HIGH); // turn OFF pump at start

    }


    bool Turn_ON() {
      lastSwitchTime=millis();
      runtimeCalc();
      if (!workingMode){     // Jeigu rezimas AUTO 
        if (ableToSwitch()){  // Ir galima perjungi (praejo daugiau nei 30min po paskutinio jungimo/isjungimo)
          digitalWrite(pin,LOW);
          ON=true;
          return 1;  
        }}
      else 
        digitalWrite(pin,LOW); // rankiu budu jungiant praeita salyga netaikoma
        ON=true;
        return 0;   
    }
    
    bool Turn_OFF() {
      lastSwitchTime=millis();
      runtimeCalc();
      if (!workingMode){
        if (ableToSwitch()){
          digitalWrite(pin,HIGH);
          ON=false;
          return 1;  
        }}
      else 
        digitalWrite(pin,HIGH);
        ON=false;
        return 0;
    }

    bool set_workingMode(bool WorkingMode){
      workingMode=WorkingMode;
    }

    String get_title(){
      return title;
    }
    bool get_state () {
      return ON;
  }

    bool get_workingMode(){
      return workingMode;
    }
  
  int get_RunTime(){
    return runtime;
  }
};



Motor Grindu_siurblys (RS_pin,"Grindu_siurblys");
Motor Radiatoriu_siurblys (GS_pin,"Radiatoriu_siurblys");
High_Temp_measure HIGH_SENSOR (MISO_pin,SCLK_pin,CS_pin);


WiFiClient espClient;
PubSubClient client(espClient);
WiFiManager wifiManager;

OneWire oneWire(DS18B20_Pin);
DallasTemperature sensors(&oneWire);

uint8_t Sensoriai [4][8]{
  { 0x28, 0xAA, 0x36, 0xA4, 0x19, 0x13, 0x02, 0xF3 },
  { 0x28, 0xFF, 0x63, 0x32, 0x00, 0x17, 0x05, 0xE5 },
  { 0x28, 0xFF, 0xD4, 0x5C, 0x00, 0x17, 0x04, 0x20 },
  { 0x28, 0xFF, 0x0F, 0x2B, 0x00, 0x17, 0x05, 0x7E },
};



void setup() {

Grindu_siurblys.init();
Radiatoriu_siurblys.init();


  
  sensors.begin();
  WiFi.persistent (false);
  Serial.begin(115200);
  Serial.println("Booting");
  
  wifiManager.setConfigPortalTimeout(200);
  wifiManager.autoConnect("Rusio_Valdiklis");

  pinMode(DS18B20_Pin, INPUT);

 // mySwitchT.enableTransmit(RF_Pin);
  //mySwitchT.setRepeatTransmit(3);
  

  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    //ESP.restart();
  }

  ArduinoOTA.setHostname("Rusys");
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  client.setServer(mqtt_broker, mqtt_port);
  client.setCallback(callback);
  while (!client.connected()) {
    String client_id = "esp32-client-";
    client_id += String(WiFi.macAddress());
    Serial.printf("The client %s connects to the public mqtt broker\n", client_id.c_str());
    if (client.connect(client_id.c_str(), mqtt_username, mqtt_password)) {
      Serial.println("mqtt broker connected");
    } else {
      Serial.print("failed with state ");
      Serial.print(client.state());
      delay(2000);
    }
  }
  // publish and subscribe
  client.subscribe("pastrevys/namai/Rusys");
      client.subscribe("pastrevys/namai/virtuve/TEMP");



}
void reconnect_WIFI(){
  
    long now = millis();
    Serial.println("Lost WIFI");
    if (now-lastReconnectAttempt_WIFI>500000){
      Serial.println("Trying to reconnect");
      lastReconnectAttempt_MQTT = now;
      Reconnect_MQTT_Attempts++;
      Serial.println("Reconnect attempts");
      Serial.println(Reconnect_MQTT_Attempts);  
      WiFi.persistent (false);
      Serial.println("Disconnect wifi");
      delay(1000);
      wifiManager.autoConnect("Rusio_Valdiklis");
      Serial.println("Autoconnect");
      delay(1000);   
    if (WiFi.status() == WL_CONNECTED){lastReconnectAttempt_WIFI=0;
     lastReconnectAttempt_WIFI = 0;
     Reconnect_WIFI_Attempts=0;
     reconnect();
     Serial.println("WIFI reconnect success");

    }}
   if (Reconnect_WIFI_Attempts>3){
    Serial.println("Reconnect unsuccessful, restarting ESP");
    ESP.restart();
          

  }  }
void reconnect_MQTT(){
  long now = millis();
  if (now - lastReconnectAttempt_MQTT > 5000) {
    lastReconnectAttempt_MQTT = now;
    Reconnect_MQTT_Attempts=+1;
  if (reconnect()) {
    lastReconnectAttempt_MQTT = 0;
    Reconnect_MQTT_Attempts=0;}  }
  if (Reconnect_MQTT_Attempts>3){
    ESP.restart();
  }
}

boolean reconnect() {
  String client_id = "Rusys";
    client_id += String(WiFi.macAddress());
  if (client.connect(client_id.c_str(), mqtt_username, mqtt_password)) {   
    client.subscribe(topic);
    client.subscribe("pastrevys/namai/Rusys");
    client.subscribe("pastrevys/namai/virtuve/TEMP");
    
  }
  return client.connected();
}

void callback(char *topic, byte *payload, unsigned int length) {
  Serial.print("Message arrived in topic: ");
  Serial.println(topic);
  Serial.print("Message:");
  for (int i = 0; i < length; i++) {
    Serial.print((char) payload[i]);
  }


  Serial.println();
  Serial.println("-----------------------");
    


    if (!(strcmp(topic,"pastrevys/namai/virtuve/TEMP"))){
      datat += ((char)payload[0] - '0') * 10;
      datat += ((char)payload[1] - '0');
      Virtuves_temp=datat;   
      datat = 0;

    }

    else {
      datat += ((char)payload[0] - '0') * 100;
      datat += ((char)payload[1] - '0') * 10;
      datat += ((char)payload[2] - '0');
    Tasker(datat);
    datat = 0;
    }
}

void pub (int DATA, char *topikas){
    String temp_str;
    char temps[50];
    temp_str=(String)DATA;
    temp_str.toCharArray(temps, temp_str.length() + 1);
    client.publish(topikas,temps );
    Serial.print("Publishinta I ");
    Serial.print(topikas);
    Serial.print(" reiksme: ");
    Serial.println(temps);
}

void pub (float DATA, char *topikas){
    char temps[50];
    dtostrf(DATA,2,2,temps);
    client.publish(topikas,temps );
    Serial.print("Publishinta I ");
    Serial.print(topikas);
    Serial.print(" reiksme: ");
    Serial.println(temps);
}





void Tasker(int pay) {
  Serial.print("Tasker:: ");
  Serial.print(pay);
  Serial.println("");
  switch (pay) {
    case 600:       Grindu_siurblys.Turn_ON();         break;   
    case 601:       Grindu_siurblys.Turn_OFF();        break;   
    case 603:       ESP.restart();                     break;   
    case 605:       Radiatoriu_siurblys.Turn_ON();     break;   
    case 606:       Radiatoriu_siurblys.Turn_OFF();    break;   
    case 607:       Radiatoriu_siurblys.set_workingMode(0);    break;   
    case 608:       Radiatoriu_siurblys.set_workingMode(1);    break;   
    case 609:       Grindu_siurblys.set_workingMode(0);    break;   
    case 610:       Grindu_siurblys.set_workingMode(1);    break;   

  }
}
void Heartbeat(){
  if (millis()-SinceLastHeartbeat>1000){
    pub (0,"pastrevys/namai/Rusys/Heartbeat");
    SinceLastHeartbeat=millis();
  }
}






void loop() { 

  sensors.requestTemperatures();
  ArduinoOTA.handle();
  client.loop();

  Heartbeat();
  Routine1();
  Warning ();

if (Radiatoriu_siurblys.get_workingMode()) {ModeAuto();}


  if ((!client.connected()) and (WiFi.status() == WL_CONNECTED) ) {
  reconnect_MQTT();}

  if (WiFi.status() != WL_CONNECTED){
    reconnect_WIFI();
  // jei wifi atsijunges pradedam prisijungima vel.
  }
}



bool WritePumpStatus(){
  
  if (Radiatoriu_siurblys.get_state()) {pub (1,"pastrevys/namai/Rusio/RSbusena");}
  else {pub (2,"pastrevys/namai/Rusio/RSbusena");}
  delay(100);
  if (Radiatoriu_siurblys.get_workingMode()) {pub (3,"pastrevys/namai/Rusio/RSbusena");}
  else {pub (4,"pastrevys/namai/Rusio/RSbusena");}
  delay(100);

  if (Grindu_siurblys.get_workingMode()) {pub (5,"pastrevys/namai/Rusio/GSbusena");}
  else {pub (6,"pastrevys/namai/Rusio/GSbusena");}
  delay(100);

  if (Grindu_siurblys.get_state()){ pub (7,"pastrevys/namai/Rusio/GSbusena");}
  else {pub (8,"pastrevys/namai/Rusio/GSbusena");}
  delay(100);
  
}

void Routine1(){
  if(millis()-Timer1 > 4000) {
    Timer1=millis();
    WritePumpStatus();
    WriteTemperatures();
    Dumtraukio_temp=HIGH_SENSOR.ReadTemp();


  }
}

void Warning (){
  if (T2>80) pub (1,"pastrevys/namai/Rusio/avarinis");
}

bool BlockPump_Cold(){
  uint8_t histerize=5;
  if (T1<=30-histerize) return 1;
  else if (T1>=30+histerize)  return 0;
}



bool ModeAuto()
{

if (BlockPump_Cold()) {Radiatoriu_siurblys.Turn_OFF();}
  else if (Virtuves_temp>=21) { Radiatoriu_siurblys.Turn_OFF();}
    else if (Virtuves_temp<=20) {Radiatoriu_siurblys.Turn_ON();}
    
}




bool WriteTemperatures(){

      if(millis()-timeSinceLastRead > 2000) {
    
    for (int i=0; i <4; i++){   
    delay(5);
    float T=GetTemperatures(Sensoriai[i]);
    if (i==0)  pub (T,"pastrevys/namai/Rusio/TEMP1");     T1=(int)T;
    //AKUMULIACINE
    delay(50);
    if (i==1)  pub (T,"pastrevys/namai/Rusio/TEMP2");     T2=(int)T;
    //IS KATILO
    delay(50);

    if (i==2)  pub (T,"pastrevys/namai/Rusio/TEMP3"); // APLINKA
    delay(50);

    if (i==3)  pub (T,"pastrevys/namai/Rusio/TEMP4"); //GRIZTA I KATILA
    }
    pub(Dumtraukio_temp,"TEMP5");
      timeSinceLastRead=millis();
      return true;
    }
}




float GetTemperatures(DeviceAddress deviceAddress)
{
  return sensors.getTempC(deviceAddress);

}

