#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <EEPROM.h> 
#include "HX711.h"

#define EEPROM_SIZE 512
#define SSID_ADDR 0
#define PASS_ADDR 32
#define MACHINE_ID 64
#define SLOT_ADDR_START 100
#define BYTES_PER_SLOT 4
#define SLOT_ADDR_END 200

// --- Dispensing State Variables ---
bool isDispensing = false;
unsigned long dispenseStartTime = 0;
int activeSlot = -1;
int activePin = -1;
int currentQtyToReduce = 0;
char currentDispenseMode = 'T'; // 'T' for Time, 'W' for Weight
int currentTargetValue = 0;     // Could be ms, could be grams

//MQTT settings
const char* mqtt_server = "46.202.167.106";
const int mqtt_port = 1884;
const char* mqtt_user = "vending_device_001";
const char* mqtt_pass = "Ras@9142422433";

WiFiClient espClient;
PubSubClient client(espClient);
TaskHandle_t mqttTask;
bool firstPublishDone = false;

#define LED_BUILTIN 2
#define AUDIO_TRIGGER_PIN   33

// MOTOR pin mapping 
//slot 1-> GPIO 4, slot2 -> GPIO 5, slot3 -> GPIO 18, slot4 -> GPIO 19
//Index 0 is -1 because slots start at 1
const int slotPins[] = { -1, 4, 5, 18,19 };
const int totalSlots = 4;

String macAddress;
String statusTopic; 
String dispenseTopic;

//webserver for provisioning
WebServer server(80);

//global buffers
char ssid[32];
char password[32];
char machineId[32];

// --- Load Cell (HX711) Settings ---
const int LOADCELL_DOUT_PIN = 21;
const int LOADCELL_SCK_PIN = 22;
HX711 scale;
float calibration_factor = 98.1;

// --- Power Cut Recovery Addresses ---
#define JOB_ACTIVE_ADDR 210
#define JOB_SLOT_ADDR 211
#define JOB_MODE_ADDR 215
#define JOB_TARGET_ADDR 216
#define JOB_QTY_ADDR 220

unsigned long lastEEPROMWriteTime = 0;
const unsigned long eepromWriteInterval = 5000; // Create a save point every 5 seconds

//save WiFi credentials to EEPROM
void saveCredentials(const char* newSSID, const char* newPassword, const char* newMachineId){  
  Serial.print("Saving WiFi credentials to EEPROM...");
  for (int i = 0; i < 32; i++){
    EEPROM.write(SSID_ADDR + i, newSSID[i]);
    EEPROM.write(PASS_ADDR + i, newPassword[i]);
    EEPROM.write(MACHINE_ID + i, newMachineId[i]);
  }
  EEPROM.commit();
  Serial.print("saved SSID:");Serial.println(newSSID);
}

// load WiFi credentials from EEPROM
void loadCredentials(){
  for(int i = 0; i < 32; i++){
    ssid[i] = EEPROM.read(SSID_ADDR + i);
    password[i] = EEPROM.read(PASS_ADDR + i);
    machineId[i] = EEPROM.read(MACHINE_ID + i);
  }
  ssid[31] = '\0';
  password[31] = '\0';
  machineId[31] = '\0';

  // Clean up any garbage characters in Machine ID if empty
  if(machineId[0] == 0xFF) strcpy(machineId, "unknown");
}

// Clear WiFi credentials
void clearCredentials(){
  for ( int i = 0; i < 100; i++){
    EEPROM.write(SSID_ADDR + i, 0xFF);
  }
  EEPROM.commit();
  Serial.print("Cleared from EEPROM");
} 

// start softAP for provisioning
void startProvisioning (){
  delay(200);
  String apSSID = "OPULANCE-" + WiFi.softAPmacAddress();
  WiFi.softAP(apSSID.c_str());
  Serial.println("Started softAP: ");Serial.print(apSSID);
  Serial.println(WiFi.softAPIP());

  //Endpoint: provision WiFi 
  server.on("/vending", HTTP_POST,[]() {
    String body = server.arg("plain");
    StaticJsonDocument<200> doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err){
      server.send(400,"application/json", "{\"error\":\"Invalid JSON\"}");
      return;
    }

    String newSSID = doc["ssid"];
    String newPASS = doc["password"];
    String newID   = doc["machine_id"]; 

    if(newID.length() > 0){
      saveCredentials(newSSID.c_str(), newPASS.c_str(), newID.c_str());
      server.send(200,"application/json", "{\"status\":\"ok\"}");
      delay(2000);
      ESP.restart();
    }else{
      server.send(400, "application/json", "{\"error\" : \"MISSING MACHINE ID\"}");
    }
  });

  //Endpoint: reset WiFi credentials
  server.on("/reset", HTTP_GET, [](){
    clearCredentials();
    server.send(200, "application/json", "{\"status\":\"reset\"}");
    delay(2000);
    ESP.restart();
  });

  server.begin();
}
// MQTT task (on core 0)
void mqttFunction(void *parameters) {
  for (;;) {

    if (WiFi.status() == WL_CONNECTED) {

      if (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        String clientId = "esp32-" + macAddress;
        digitalWrite(LED_BUILTIN, LOW);

        if (client.connect(
          clientId.c_str(),
          mqtt_user,
          mqtt_pass,
          statusTopic.c_str(),      // LWT topic
          1,                        // QoS
          true,                     // retain
          "{\"state\":\"OFFLINE\"}" // LWT payload
        )) {
          Serial.println("connected to MQTT");
          digitalWrite(LED_BUILTIN, HIGH);
          client.subscribe(("project/vending/"+ macAddress + "/refill").c_str());
          client.subscribe(("project/vending/"+ macAddress +"/check").c_str());
          client.subscribe(dispenseTopic.c_str());

          String onlinePayload = "{";
          onlinePayload += "\"state\":\"ONLINE\",";
          onlinePayload += "\"fw\":\"1.0.0\",";
          onlinePayload += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
          onlinePayload += "}";

          client.publish(statusTopic.c_str(),onlinePayload.c_str(),true);   // retain = true

          if (!firstPublishDone) {
            String payload = "{";
            payload += "\"mac\":\"" + macAddress + "\",";
            payload += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
            payload += "\"SSID\":\""+ WiFi.SSID()+"\",";
            payload += "\"Password\":\"" +String(password)+"\",";
            payload += "\"fw_version\":\"1.0.0\"";
            payload += "}";

            client.publish(
              ("project/vending/" + macAddress + "/register").c_str(),
              payload.c_str()
            );
            Serial.println("Published MAC info :" + payload);
            firstPublishDone = true;
          }

        } else {
          Serial.print(" failed, rc=");
          Serial.println(client.state());
          digitalWrite(LED_BUILTIN, LOW);
          vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
      }

      client.loop();
      vTaskDelay(10 / portTICK_PERIOD_MS);

    } else {
      // WiFi not connected
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
  }
}

//Save quantity corresponding to each slot in EEPROM for liquid(mL) and solid
void updateStock(int slotNumber,int amountToAdd){
  // Address
  int address = SLOT_ADDR_START + ((slotNumber - 1) * BYTES_PER_SLOT);

  // check address is between the limit
  if(address + BYTES_PER_SLOT > SLOT_ADDR_END){
    Serial.print("Slot ERROR");
    Serial.print(slotNumber);
    Serial.print("is outside memory Range");
    return;
  }

  // Read current Stock
  int currentStock ;
  EEPROM.get(address, currentStock);

  // Fix Garbage  data
  if(currentStock < 0 || currentStock > 10000) currentStock = 0;

  // Update stock
  int newTotal = currentStock + amountToAdd;

  //Save back to EEPROM
  EEPROM.put(address, newTotal);
  EEPROM.commit();

  Serial.print("Slot ");Serial.print(slotNumber);
  Serial.print("Updated to ");Serial.print(newTotal);
  Serial.print(" (oldStock : ");Serial.print(currentStock);
  Serial.print(" + New Stock : ");Serial.print(amountToAdd);
}

//updated Stock Check 
int getStock(int slotNumber){
  int address = SLOT_ADDR_START + ((slotNumber - 1) * BYTES_PER_SLOT);

  //Safety check
  if(address + BYTES_PER_SLOT > SLOT_ADDR_END){
    Serial.print("Invalid Slot Number");
    return 0;
  }

  int currentStock;
  EEPROM.get(address, currentStock);

  if(currentStock < 0 || currentStock > 100000) return 0;

  return currentStock;
}

// dispensing function
void dispenseItem(int slot, char mode, int targetValue, int amountToReduce) {
  // 1. Check if machine is already busy
  if (isDispensing) {
    Serial.println("Machine is busy dispensing. Ignoring new command.");
    client.publish(statusTopic.c_str(), "{\"event\":\"error\", \"msg\":\"machine_busy\"}");
    return;
  }

  // 2. Safety check for slot limits
  if(slot < 1 || slot > totalSlots){
    Serial.printf("Invalid Slot: %d\n", slot);
    client.publish(statusTopic.c_str(), "{\"event\":\"error\", \"msg\":\"invalid_slot\"}");
    return;
  }

  // 3. Stock Check
  int currentStock = getStock(slot);
  if(currentStock < amountToReduce){
    Serial.printf("Insufficient Stock (Has: %d, Need: %d)\n", currentStock, amountToReduce);
    String errMsg = "{\"event\":\"error\", \"type\":\"out_of_stock\", \"slot\":" + String(slot) + "}";
    client.publish(statusTopic.c_str(), errMsg.c_str());
    return;
  }

  // 4. Set the Global Variables for the background loop
  activeSlot = slot;
  activePin = slotPins[slot];
  currentDispenseMode = mode;           // NEW: Save the mode
  currentTargetValue = targetValue;     // NEW: Save the target
  currentQtyToReduce = amountToReduce;

  Serial.printf("STARTING Dispense Slot %d (Pin %d). Mode: %c, Target: %d\n", slot, activePin, mode, targetValue);

  // NEW: If we are using weight mode, zero the scale BEFORE pouring!
  if (currentDispenseMode == 'W') {
    scale.tare(); 
    Serial.println("Scale tared to 0. Ready to pour liquid.");
  }

  // Trigger Audio
  digitalWrite(AUDIO_TRIGGER_PIN, HIGH);
  delay(100); // A 100ms delay for a quick audio trigger is fine, it won't break MQTT.
  digitalWrite(AUDIO_TRIGGER_PIN, LOW);

  // 5. Turn Motor/Relay ON and start the clock!
  digitalWrite(activePin, HIGH);
  isDispensing = true;
  dispenseStartTime = millis(); // Record the exact millisecond it started

  saveJobState(true, currentTargetValue);
}

void handleDispensing() {
  if (isDispensing) {
    bool targetReached = false;
    int remainingTarget = currentTargetValue; // For EEPROM saves

    // Mode: TIME ('T')
    if (currentDispenseMode == 'T') {
      unsigned long elapsedTime = millis() - dispenseStartTime;
      remainingTarget = currentTargetValue - elapsedTime; // Calculate remaining time
      
      if (elapsedTime >= currentTargetValue) {
        targetReached = true;
      }
    } 
    // Mode: WEIGHT ('W')
    else if (currentDispenseMode == 'W') {
      if (scale.is_ready()) {
        float currentWeight = scale.get_units(1); 
        remainingTarget = currentTargetValue - currentWeight; // Calculate remaining weight
        
        if (currentWeight >= currentTargetValue) {
          targetReached = true;
        }
      }
    }
    // Mode: COUNT ('C')
    else if (currentDispenseMode == 'C') {
       targetReached = true; 
    }
    // Mode: ENERGY ('E')
    else if (currentDispenseMode == 'E') {
       targetReached = true; 
    }

    // --- NEW: Save Checkpoint every 5 seconds ---
    if (!targetReached && (millis() - lastEEPROMWriteTime >= eepromWriteInterval)) {
        saveJobState(true, remainingTarget);
        lastEEPROMWriteTime = millis();
        Serial.printf("Checkpoint saved. Remaining: %d\n", remainingTarget);
    }

    // IF THE TARGET IS REACHED...
    if (targetReached) {
      
      // 1. Turn off the motor/relay
      digitalWrite(activePin, LOW);
      isDispensing = false; 
      Serial.println("Dispense complete");

      // 2. NEW: CLEAR THE SAVED JOB FROM MEMORY!
      saveJobState(false, 0); 

      // 3. Post-dispense updates (DB and MQTT)
      updateStock(activeSlot, -currentQtyToReduce);
      int newStock = getStock(activeSlot);

      String msg = "{\"event\":\"dispensed\", \"slot\":" + String(activeSlot) + ", \"remaining\":" + String(newStock) + "}";
      client.publish(statusTopic.c_str(), msg.c_str());

      // 4. Low stock warnings
      int stockThreshold = (currentQtyToReduce <= 3 && currentQtyToReduce > 0) ? 5 : 400;
      if(newStock <= stockThreshold){
        String warnMsg = "{\"event\":\"warning\", \"type\":\"low_stock\", \"slot\":" + String(activeSlot) + ", \"remaining\":" + String(newStock) + "}";
        client.publish(statusTopic.c_str(), warnMsg.c_str());
      }

      // 5. Reset variables for safety
      activeSlot = -1;
      activePin = -1;
    }
  }
}

// Saves the current job to memory so it survives a reboot
void saveJobState(bool active, int remainingTarget) {
  EEPROM.write(JOB_ACTIVE_ADDR, active ? 1 : 0);
  
  if (active) {
    EEPROM.put(JOB_SLOT_ADDR, activeSlot);
    EEPROM.write(JOB_MODE_ADDR, currentDispenseMode);
    EEPROM.put(JOB_TARGET_ADDR, remainingTarget);
    EEPROM.put(JOB_QTY_ADDR, currentQtyToReduce);
  }
  EEPROM.commit();
}

// Checks if a job was running when the power died
void checkRecovery() {
  byte active = EEPROM.read(JOB_ACTIVE_ADDR);
  
  if (active == 1) {
    Serial.println("!!! POWER CUT DETECTED !!! Recovering unfinished job...");
    
    // Read the saved variables
    EEPROM.get(JOB_SLOT_ADDR, activeSlot);
    currentDispenseMode = EEPROM.read(JOB_MODE_ADDR);
    EEPROM.get(JOB_TARGET_ADDR, currentTargetValue);
    EEPROM.get(JOB_QTY_ADDR, currentQtyToReduce);
    
    activePin = slotPins[activeSlot];
    
    // Prepare the hardware
    if (currentDispenseMode == 'W') {
      scale.tare(); // Zero the scale before resuming liquid
    }
    
    // Turn the relay back ON and restart the clock!
    digitalWrite(activePin, HIGH);
    isDispensing = true;
    dispenseStartTime = millis();
    lastEEPROMWriteTime = millis();
    
    Serial.printf("Resumed Slot %d. Mode: %c. Remaining Target: %d\n", activeSlot, currentDispenseMode, currentTargetValue);
  }
}

void callback(char* topic, byte* payload, unsigned int length){
  // Create a clean copy of the incoming payload
  char value[length + 1];
  strncpy(value, (char*)payload, length);
  value[length] = '\0';

  // 1. Refilling Topic 
  if(strcmp(topic,("project/vending/"+ macAddress +"/refill").c_str()) == 0){
    char* token = strtok(value, "/");
    if(token != NULL){
      int slot = atoi(token); 
      token = strtok(NULL, "/");
      if(token != NULL){
        int amount = atoi(token); 
        updateStock(slot, amount);
        String feedback ="{\"event\":\"refilled\", \"slot\":"+String(slot)+", \"new_total\":"+String(getStock(slot))+"}";
        client.publish(statusTopic.c_str(), feedback.c_str());
      }
    }
  }

  // 2. Stock Check Topic
  if(strcmp(topic,("project/vending/"+ macAddress +"/check").c_str()) == 0){
    int slot = atoi(value);
    int currentStock = getStock(slot);
    String msg = "{\"event\":\"stock_check\", \"slot\":"+String(slot)+", \"level\":"+String(currentStock)+"}";
    client.publish(statusTopic.c_str(), msg.c_str());
    Serial.println(msg);
  }

  // 3. UNIVERSAL Dispense Topic 
  // Expected Format: Slot/Mode/Target/QtyToReduce
  if(strcmp(topic, dispenseTopic.c_str()) == 0){
    
    // Extract Slot
    char* token = strtok(value, "/");
    if(token != NULL){
      int slot = atoi(token);
      
      // Extract Mode ('T' for Time, 'W' for Weight)
      token = strtok(NULL, "/");
      if(token != NULL){
        char mode = token[0]; 

        // Extract Target Value (Milliseconds OR Grams)
        token = strtok(NULL, "/");
        if(token != NULL){
          int targetValue = atoi(token);

          // Extract Quantity to Reduce (Default to 1 if missing)
          token = strtok(NULL, "/");
          int qtyToReduce = 1; 
          if( token != NULL){
            qtyToReduce = atoi(token);
          }

          // Pass the new universal parameters to the dispense mechanism
          dispenseItem(slot, mode, targetValue, qtyToReduce);
        }
      }
    }
  }
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);

  // Initialize HX711 Scale
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(calibration_factor);
  scale.tare(); // Reset scale to 0 on boot

  checkRecovery();

  // Initialize Pins
  pinMode(LED_BUILTIN, OUTPUT);
  for(int i=1; i<=totalSlots; i++) {
    pinMode(slotPins[i], OUTPUT);
    digitalWrite(slotPins[i], LOW); // Ensure Motors start OFF
  }

  pinMode(AUDIO_TRIGGER_PIN, OUTPUT);
  digitalWrite(AUDIO_TRIGGER_PIN, LOW);

  //WIFI configuration
  loadCredentials();
  Serial.print("loaded SSID: "); Serial.println(ssid);
   //if no credentials start provisiong
  if(strlen(ssid) == 0 || ssid[0] == (char)0xFF || strlen(machineId) == 0 || machineId[0] == (char) 0xFF ){
    startProvisioning();
  }else{
    WiFi.begin(ssid, password);
    Serial.println("Connecting to WiFi...");
    unsigned long startAttemptTime = millis();

    while( WiFi.status() != WL_CONNECTED && millis() - startAttemptTime <20000){
      Serial.print(".");
      delay(500);
    }

    if(WiFi.status() == WL_CONNECTED){
      Serial.println("WiFi connected");
      Serial.print("IP Address: ");Serial.print(WiFi.localIP());
      macAddress = WiFi.macAddress();
      statusTopic = "project/vending/" + macAddress + "/status";
      dispenseTopic = "project/vending/" + macAddress + "/dispense";

      //connect to MQTT
      client.setServer(mqtt_server, mqtt_port);
      client.setCallback(callback);

      //MQTT task on core 1
      xTaskCreatePinnedToCore(
        mqttFunction, // function
        "mqttTask",// name
        10000, // stack Size
        NULL, //parameter
        1, //priority
        &mqttTask, //task handle
        1 // core
      );
    } else {
      startProvisioning();
    }
  } 
}

void loop() {
  if(WiFi.getMode() == WIFI_AP) {
    server.handleClient();
    return;
  }
 handleDispensing();
}
