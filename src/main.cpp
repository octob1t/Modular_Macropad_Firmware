#include "FS.h"
#include "SPIFFS.h"
#include "ArduinoJson.h"
#include "USB.h"

// Include your project modules
#include "ModuleSetup.h"
#include "DisplayHandler.h"
#include "EncoderHandler.h"
#include "LEDHandler.h"
#include "ConfigManager.h"
#include "KeyHandler.h"
#include "HIDHandler.h"
#include <tusb.h> 

//  Will be moved to info.json
#define ROW0 3  // Kept (safe GPIO pin)
#define ROW1 5  // Kept (safe GPIO pin)
#define ROW2 8  // ROW2 26
#define ROW3 9  // ROW3 47
#define ROW4 10 // ROW4 33
#define COL0 11 // COL0 34
#define COL1 21 // Kept (safe GPIO pin)
#define COL2 13 // Kept (safe GPIO pin)
#define COL3 6  // Kept (safe GPIO pin)
#define COL4 12 // Kept (safe GPIO pin)

// Global static variable to cache encoder actions (loaded once)
static std::map<String, ActionConfig> encoderActions;
static bool encoderActionsLoaded = false;

void directKeyboardScan() {
  uint8_t rowPins[5] = {ROW0, ROW1, ROW2, ROW3, ROW4};
  uint8_t colPins[5] = {COL0, COL1, COL2, COL3, COL4};
  
  // First set all pins to a known state
  for (int r = 0; r < 5; r++) {
    pinMode(rowPins[r], INPUT_PULLUP);
  }
  
  for (int c = 0; c < 5; c++) {
    pinMode(colPins[c], INPUT);  // High impedance when not scanning
  }
  
  // Scan one column at a time with verbose debugging
  for (int c = 0; c < 5; c++) {
    // Explicitly set this column as OUTPUT and drive it LOW
    pinMode(colPins[c], OUTPUT);
    digitalWrite(colPins[c], LOW);
    
    // Verify the pin is actually LOW
    Serial.printf("Setting column %d (pin %d) LOW. Voltage should be 0V\n", 
                  c, colPins[c]);
    
    // Longer delay to allow measurement
    delay(1000);
    
    // Check all rows
    for (int r = 0; r < 5; r++) {
      int rowValue = digitalRead(rowPins[r]);
      Serial.printf("Row %d (pin %d): %s\n", 
                    r, rowPins[r], rowValue == LOW ? "PRESSED" : "not pressed");
    }
    
    // Reset column to high impedance
    pinMode(colPins[c], INPUT);
    
    // Allow time between columns
    delay(500);
  }
}

void testBasicGPIO() {
  // Clear all previous pin settings
  for(int i=0; i<22; i++) {
    pinMode(i, INPUT);
  }
  
  // Test a single GPIO pin that isn't part of your matrix
  const int testPin = 6; // Choose an unused GPIO pin
  
  pinMode(testPin, OUTPUT);
  
  // Toggle between HIGH and LOW every second
  while(true) {
    digitalWrite(testPin, LOW);
    Serial.println("Pin set to LOW - measure now");
    delay(2000);
    
    digitalWrite(testPin, HIGH);
    Serial.println("Pin set to HIGH - measure now");
    delay(2000);
  }
}

// Function to set all columns LOW for testing
void setAllColumnsLow() {
  uint8_t colPins[5] = {COL0, COL1, COL2, COL3, COL4};
  
  Serial.println("Setting all columns LOW for testing...");
  
  // Configure columns as OUTPUT
  for (int c = 0; c < 5; c++) {
    pinMode(colPins[c], OUTPUT);
    digitalWrite(colPins[c], LOW);  // Set LOW
    Serial.printf("Column %d (pin %d) set to LOW\n", c, colPins[c]);
  }
  
  Serial.println("All columns set to LOW. You can now measure each pin.");
}

// Function to list files in SPIFFS directory (for debugging)
void listDir(fs::FS &fs, const char* dirname, uint8_t levels) {
  Serial.printf("Listing directory: %s\n", dirname);
  File root = fs.open(dirname);
  if (!root) {
    Serial.println("Failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    Serial.println("Not a directory");
    return;
  }
  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("  DIR : ");
      Serial.println(file.name());
      if (levels) {
        listDir(fs, file.name(), levels - 1);
      }
    } else {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("\tSIZE: ");
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}

// Improved key mapping creation from components.json with no character overlaps
char** createKeyMappingFromComponents(const String& componentsJson, uint8_t rows, uint8_t cols) {
    // Allocate the key mapping grid
    char** keyMapping = new char*[rows];
    for (uint8_t i = 0; i < rows; i++) {
        keyMapping[i] = new char[cols];
        for (uint8_t j = 0; j < cols; j++) {
            keyMapping[i][j] = 'X';  // Initialize all positions as 'X'
        }
    }

    // Parse the components JSON
    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(doc, componentsJson);
    
    if (error) {
        Serial.print("Error parsing components JSON: ");
        Serial.println(error.c_str());
        return keyMapping;
    }

    // Iterate through components
    JsonArray components = doc["components"].as<JsonArray>();
    
    for (JsonObject component : components) {
        String type = component["type"].as<String>();
        String id = component["id"].as<String>();
        
        // Check if component is a button or an encoder with a button
        if (type == "button" || 
            (type == "encoder" && 
             component.containsKey("with_button") && 
             component["with_button"].as<bool>())) {
            
            // Get the component's location
            uint8_t startRow = component["start_location"]["row"];
            uint8_t startCol = component["start_location"]["column"];
            
            // Extract the component number and create a unique key character
            char keyChar = 'X';
            
            if (type == "button") {
                // For buttons, extract number from id (e.g., "button-1" -> '1')
                int dashPos = id.indexOf('-');
                if (dashPos >= 0 && dashPos < id.length() - 1) {
                    String numStr = id.substring(dashPos + 1);
                    int buttonNum = numStr.toInt();
                    
                    if (buttonNum > 0 && buttonNum <= 9) {
                        // Buttons 1-9 -> '1'-'9'
                        keyChar = '0' + buttonNum;
                    } else if (buttonNum >= 10 && buttonNum <= 35) {
                        // Buttons 10-35 -> 'A'-'Z'
                        keyChar = 'A' + (buttonNum - 10);
                    }
                }
            } else if (type == "encoder" && component["with_button"].as<bool>()) {
                // For encoder buttons, use special characters that won't overlap with buttons
                // Use characters outside the normal ASCII range for better separation
                int dashPos = id.indexOf('-');
                if (dashPos >= 0 && dashPos < id.length() - 1) {
                    String numStr = id.substring(dashPos + 1);
                    int encoderNum = numStr.toInt();
                    // Use 'a'-'z' range for encoders to differentiate from buttons
                    keyChar = 'a' + (encoderNum - 1);  // encoder-1 -> 'a', encoder-2 -> 'b', etc.
                }
            }
            
            // Validate the location is within grid bounds
            if (startRow < rows && startCol < cols && keyChar != 'X') {
                // Assign the key marker based on component ID
                keyMapping[startRow][startCol] = keyChar;
                
                Serial.printf("Mapped %s at [%d,%d] with key %c\n", 
                              id.c_str(), startRow, startCol, keyChar);
            }
        }
    }

    // Debug print the entire key mapping
    Serial.println("Key mapping matrix:");
    for (uint8_t i = 0; i < rows; i++) {
        Serial.print("Row ");
        Serial.print(i);
        Serial.print(": ");
        for (uint8_t j = 0; j < cols; j++) {
            Serial.print(keyMapping[i][j]);
            Serial.print(" ");
        }
        Serial.println();
    }

    return keyMapping;
}

// Validate GPIO pins for ESP32-S3
bool validateGpioPins(uint8_t* pins, uint8_t count) {
    // Valid GPIO pins for ESP32-S3 are 0-21
    const int validESP32S3Pins[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 
        15, 16, 17, 18, 19, 20, 21
    };
    
    for (uint8_t i = 0; i < count; i++) {
        bool isValid = false;
        for (int validPin : validESP32S3Pins) {
            if (pins[i] == validPin) {
                isValid = true;
                break;
            }
        }
        
        if (!isValid) {
            Serial.printf("Invalid GPIO pin for ESP32-S3: %d\n", pins[i]);
            return false;
        }
    }
    return true;
}

// Consistent pin configuration for key matrix
void configurePinModes(uint8_t* rowPins, uint8_t* colPins, uint8_t rows, uint8_t cols) {
    // Validate pins first with detailed ESP32-S3 validation
    if (!validateGpioPins(rowPins, rows) || !validateGpioPins(colPins, cols)) {
        Serial.println("Invalid GPIO pins detected for ESP32-S3!");
        return;
    }

    // IMPORTANT: For a matrix keyboard setup:
    // For a typical matrix keyboard setup:
    // - Configure rows as INPUT_PULLUP (will be read)
    // - Configure columns as OUTPUT (will be driven)
    
    // First, print the pin assignments for clarity
    Serial.println("\n--- Pin Configuration ---");
    Serial.println("Row pins (configured as INPUT_PULLUP):");
    for (uint8_t i = 0; i < rows; i++) {
        Serial.printf("  Row %d: GPIO %d\n", i, rowPins[i]);
    }
    
    Serial.println("Column pins (configured as OUTPUT):");
    for (uint8_t j = 0; j < cols; j++) {
        Serial.printf("  Column %d: GPIO %d\n", j, colPins[j]);
    }
    
    // Now configure the pins
    for (uint8_t i = 0; i < rows; i++) {
        pinMode(rowPins[i], INPUT_PULLUP);
    }

    for (uint8_t j = 0; j < cols; j++) {
        pinMode(colPins[j], OUTPUT);
        digitalWrite(colPins[j], HIGH);  // Start with HIGH (inactive)
    }
    
    Serial.println("Pin configuration complete\n");
}

// Function to debug actions configuration
void debugActionsConfig() {
    auto actions = ConfigManager::loadActions("/config/actions.json");
    
    Serial.println("\n=== Actions Configuration Debug ===");
    for (const auto& pair : actions) {
        Serial.printf("Button ID: %s, Type: %s\n", 
                    pair.first.c_str(), 
                    pair.second.type.c_str());
        
        if (pair.second.type == "multimedia" && !pair.second.consumerReport.empty()) {
            Serial.print("  Consumer Report: ");
            for (const auto& hex : pair.second.consumerReport) {
                Serial.printf("%s ", hex.c_str());
            }
            Serial.println();
        }
    }
    Serial.println("==================================\n");
}

// initializeKeyHandler() using a 5x5 grid and loading actions via ConfigManager
void initializeKeyHandler() {
  const uint8_t rows = 5;
  const uint8_t cols = 5;
  uint8_t rowPins[rows] = {ROW0, ROW1, ROW2, ROW3, ROW4};
  uint8_t colPins[cols] = {COL0, COL1, COL2, COL3, COL4};
  
  Serial.println("\n=== Initializing Keyboard Matrix ===");
  Serial.println("Matrix dimensions: 5x5");
  
  // Log pin assignments for clarity  
  Serial.println("Row pins:");
  for (int i = 0; i < rows; i++) {
    Serial.printf("  Row %d: GPIO %d\n", i, rowPins[i]);
  }
  
  Serial.println("Column pins:");
  for (int i = 0; i < cols; i++) {
    Serial.printf("  Column %d: GPIO %d\n", i, colPins[i]);
  }
  
  // Configure pin modes
  configurePinModes(rowPins, colPins, rows, cols);
  
  // Read components JSON from the file
  String componentsJson = ConfigManager::readFile("/config/components.json");
  Serial.println("Loading components from JSON...");
  
  // Create key mapping dynamically from components
  Serial.println("Creating key mapping from components...");
  char** keyMapping = createKeyMappingFromComponents(componentsJson, rows, cols);
  
  // Create and initialize the key handler instance
  Serial.println("Initializing key handler instance...");
  keyHandler = new KeyHandler(rows, cols, keyMapping, rowPins, colPins);
  
  if (keyHandler) {
    keyHandler->begin();
    
    // Load the actions configuration from SPIFFS
    Serial.println("Loading key action configuration...");
    auto actions = ConfigManager::loadActions("/config/actions.json");
    keyHandler->loadKeyConfiguration(actions);
    
    // Debug the loaded actions configuration
    debugActionsConfig();
    
    Serial.println("Key handler initialization complete");
  } else {
    Serial.println("ERROR: Failed to create key handler instance!");
  }
  
  // Clean up temporary keyMapping allocation
  for (uint8_t i = 0; i < rows; i++) {
    delete[] keyMapping[i];
  }
  delete[] keyMapping;
  
  Serial.println("=== Keyboard Matrix Initialization Complete ===\n");
}

void customKeyHandler() {
  static unsigned long lastScan = 0;
  const unsigned long scanInterval = 20; // ms
  
  // Only scan at regular intervals
  if (millis() - lastScan < scanInterval) {
    return;
  }
  lastScan = millis();
  
  uint8_t rowPins[5] = {ROW0, ROW1, ROW2, ROW3, ROW4};
  uint8_t colPins[5] = {COL0, COL1, COL2, COL3, COL4};
  
  // For tracking key state changes and debouncing
  static bool keyStates[5][5] = {0};
  static unsigned long lastDebounceTime[5][5] = {0};
  
  // IMPORTANT: Reverse the roles of rows and columns
  // This inverts the scanning direction to work better with diodes
  
  // Configure rows as OUTPUT initially (instead of columns)
  for (int r = 0; r < 5; r++) {
    pinMode(rowPins[r], OUTPUT);
    digitalWrite(rowPins[r], HIGH); // Start with HIGH (inactive)
  }
  
  // Configure columns as INPUT_PULLUP (instead of rows)
  for (int c = 0; c < 5; c++) {
    pinMode(colPins[c], INPUT_PULLUP);
  }
  
  // Scan one row at a time (reverse of normal scanning)
  for (int r = 0; r < 5; r++) {
    // Drive this row LOW
    digitalWrite(rowPins[r], LOW);
    
    delayMicroseconds(50); // Allow signal to stabilize
    
    // Check all columns
    for (int c = 0; c < 5; c++) {
      // Read the column pin
      bool currentReading = (digitalRead(colPins[c]) == LOW);
      
      // Debouncing
      if (currentReading != keyStates[r][c]) {
        lastDebounceTime[r][c] = millis();
      }
      
      if ((millis() - lastDebounceTime[r][c]) > DEBOUNCE_TIME) {
        if (currentReading != keyStates[r][c]) {
          keyStates[r][c] = currentReading;
          
          if (currentReading) {  // Key pressed
            // Map to button ID - this might need adjustment for your layout
            String buttonId = "button-" + String(r * 5 + c + 1);
            
            // Override for specific positions if needed
            if (r == 0 && c == 3) buttonId = "button-1";
            
            Serial.printf("Key press: Row %d, Col %d, ID=%s\n", r, c, buttonId.c_str());
            syncLEDsWithButtons(buttonId.c_str(), true);
          } else {  // Key released
            String buttonId = "button-" + String(r * 5 + c + 1);
            if (r == 0 && c == 3) buttonId = "button-1";
            
            Serial.printf("Key release: Row %d, Col %d, ID=%s\n", r, c, buttonId.c_str());
            syncLEDsWithButtons(buttonId.c_str(), false);
          }
        }
      }
    }
    
    // Set row back to inactive
    digitalWrite(rowPins[r], HIGH);
  }
}

void safeButtonTest() {
  // Only test one specific button to avoid hardware conflicts
  // Button-1 at row 0, column 3
  uint8_t rowPin = ROW0;  // 3
  uint8_t colPin = COL4;  // 12
  
  // Configure pins carefully
  pinMode(rowPin, INPUT_PULLUP);
  pinMode(colPin, OUTPUT);
  digitalWrite(colPin, LOW);
  
  // Read state
  int reading = digitalRead(rowPin);
  
  // Show result
  Serial.printf("Button test (standard): %s\n", 
               reading == LOW ? "PRESSED" : "NOT PRESSED");
  
  // Reset pins to safe state
  pinMode(rowPin, INPUT);
  pinMode(colPin, INPUT);
  
  // Try reversed configuration
  delay(100);
  pinMode(colPin, INPUT_PULLUP);
  pinMode(rowPin, OUTPUT);
  digitalWrite(rowPin, LOW);
  
  // Read state
  reading = digitalRead(colPin);
  
  // Show result
  Serial.printf("Button test (reversed): %s\n", 
               reading == LOW ? "PRESSED" : "NOT PRESSED");
  
  // Reset pins to safe state
  pinMode(rowPin, INPUT);
  pinMode(colPin, INPUT);
  
  delay(1000);
}

// Modified initializeEncoderHandler remains as before
void initializeEncoderHandler() {
    // Read components JSON from the file
    String componentsJson = ConfigManager::readFile("/config/components.json");
    Serial.println("Loading components from JSON for encoders...");
    
    // Parse the components to get encoder configurations
    std::vector<Component> components = ConfigManager::loadComponents("/config/components.json");
    
    // Count encoders
    uint8_t encoderCount = 0;
    for (const Component& comp : components) {
        if (comp.type == "encoder") {
            encoderCount++;
        }
    }
    
    Serial.printf("Found %d encoders in configuration\n", encoderCount);
    
    // Create handler if we have encoders
    if (encoderCount > 0) {
        encoderHandler = new EncoderHandler(encoderCount);
        
        // Configure each encoder
        uint8_t encoderIndex = 0;
        for (const Component& comp : components) {
            if (comp.type == "encoder") {
                // Use a larger capacity for JSON parsing here
                DynamicJsonDocument doc(8192);
                DeserializationError error = deserializeJson(doc, componentsJson);
                if (error) {
                    Serial.printf("Error parsing components JSON: %s\n", error.c_str());
                    continue;
                }
                
                // Find this encoder in the parsed JSON
                JsonArray jsonComponents = doc["components"].as<JsonArray>();
                JsonObject encoderConfig;
                for (JsonObject component : jsonComponents) {
                    if (component["id"].as<String>() == comp.id) {
                        encoderConfig = component;
                        break;
                    }
                }
                
                if (!encoderConfig.isNull()) {
                    // Determine encoder type (default mechanical unless configured as as5600)
                    EncoderType type = ENCODER_TYPE_MECHANICAL;
                    if (encoderConfig.containsKey("configuration") && 
                        encoderConfig["configuration"].containsKey("type") &&
                        encoderConfig["configuration"]["type"].as<String>() == "as5600") {
                        type = ENCODER_TYPE_AS5600;
                    }
                    
                    // Get pins and configuration
                    uint8_t pinA = 0, pinB = 0;
                    int8_t direction = 1;
                    
                    if (encoderConfig.containsKey("mechanical")) {
                        pinA = encoderConfig["mechanical"]["pin_a"] | 0;
                        pinB = encoderConfig["mechanical"]["pin_b"] | 0;
                    }
                    
                    if (encoderConfig.containsKey("configuration") && 
                        encoderConfig["configuration"].containsKey("direction")) {
                        direction = encoderConfig["configuration"]["direction"] | 1;
                    }
                    
                    // Configure this encoder
                    Serial.printf("Configuring %s: type=%d, pinA=%d, pinB=%d, direction=%d\n", 
                                  comp.id.c_str(), type, pinA, pinB, direction);
                    
                    encoderHandler->configureEncoder(
                        encoderIndex++,
                        type,
                        pinA,
                        pinB,
                        direction,
                        0 // zeroPosition
                    );
                }
            }
        }
        
        // Initialize the configured encoders
        encoderHandler->begin();
        Serial.println("Encoder handler initialized successfully");
    } else {
        Serial.println("No encoders found in configuration");
    }
}

bool testI2CPins(uint8_t sdaPin, uint8_t sclPin) {
    // End any previous I2C communication
    Wire.end();
    
    // Try to initialize I2C on the specified pins
    Wire.begin(sdaPin, sclPin);
    
    // Attempt an I2C scan (this won't find anything but tests if pins can be configured)
    Wire.beginTransmission(0x36); // AS5600 address
    uint8_t error = Wire.endTransmission();
    
    // We expect error 2 (received NACK on transmit of address) if no device is present
    // but pins can be used for I2C. Any other error might indicate pin configuration problems.
    Serial.printf("Testing I2C pins SDA=%d, SCL=%d: ", sdaPin, sclPin);
    if (error == 2) {
        Serial.println("Pins can be used for I2C (no device found, as expected)");
        return true;
    } else if (error == 0) {
        Serial.println("Unexpectedly received ACK - is a device connected?");
        return true; // Pins are working
    } else {
        Serial.printf("Error %d - pins might not be usable for I2C\n", error);
        return false;
    }
}

// Ensure USB pins are clear for USB operation
void prepareUsbPins() {
    // ESP32-S3 USB pins
    pinMode(19, INPUT);  // USB D-
    pinMode(20, INPUT);  // USB D+
    
    // Clear any interference
    digitalWrite(19, LOW);
    digitalWrite(20, LOW);
}

void setup() {
    // Start serial communication with long timeout
    Serial.begin(115200);
    delay(3000);
    Serial.println("\n\n=== Starting Device Initialization ===");
    
    // Prepare USB pins and initialize USB first
    prepareUsbPins();
    
    Serial.println("Initializing USB...");
    USB.begin();
    delay(500);
    
    // Process USB tasks to help with enumeration
    Serial.println("Processing initial USB tasks...");
    for (int i = 0; i < 20; i++) {
        tud_task();
        delay(10);
    }
    
    // Mount SPIFFS with error handling
    if (!SPIFFS.begin(false)) {
        Serial.println("SPIFFS mount failed!");
        if (SPIFFS.format()) {
            Serial.println("SPIFFS formatted successfully");
            if (!SPIFFS.begin(false)) {
                Serial.println("Failed to mount SPIFFS after formatting");
                return;
            }
        } else {
            Serial.println("SPIFFS format failed");
            return;
        }
    } else {
        Serial.println("SPIFFS mounted successfully.");
    }
    
    // List files for debugging
    listDir(SPIFFS, "/", 0);
    
    // Initialize module-specific setups
    Serial.println("Initializing module info...");
    initializeModuleInfo();
    
    Serial.println("Initializing LED handler...");
    initializeLED();
    
    Serial.println("Initializing key handler...");
    initializeKeyHandler();
    
    Serial.println("Initializing encoder handler...");
    initializeEncoderHandler();
    
    // Process USB tasks again before HID initialization
    for (int i = 0; i < 20; i++) {
        tud_task();
        delay(10);
    }
    
    // Initialize HID handler with additional debug
    Serial.println("Initializing HID handler...");
    initializeHIDHandler();
    
    // Verify HID handler was created
    if (hidHandler != nullptr) {
        Serial.println("HID handler initialized successfully (pointer is valid)");
    } else {
        Serial.println("ERROR: HID handler is NULL after initialization!");
        
        // Try to re-create HID handler directly
        Serial.println("Attempting to create HID handler directly...");
        hidHandler = new HIDHandler();
        if (hidHandler) {
            hidHandler->begin();
            Serial.println("HID handler created directly");
        } else {
            Serial.println("Failed to create HID handler directly!");
        }
    }
    
    // Process USB tasks more aggressively
    Serial.println("Processing USB tasks...");
    for (int i = 0; i < 100; i++) {
        tud_task();
        delay(10);
    }
    
    // Wait until the HID interface is ready (with a timeout)
    unsigned long start = millis();
    while (!tud_hid_ready() && (millis() - start < 5000)) {
        tud_task();  // Process USB tasks while waiting
        delay(10);
    }
    
    if (tud_hid_ready()) {
        Serial.println("HID interface is now ready.");
    } else {
        Serial.println("HID interface not ready after timeout.");
    }
    
    // Test various I2C pin combinations (for debugging AS5600 encoder if needed)
    Serial.println("\n=== Testing I2C Pin Combinations ===");
    testI2CPins(17, 18); // Example test: pins 17, 18
    testI2CPins(35, 36); // Example test: pins 35, 36
    
    delay(1000);
    Serial.println("Initialization complete!");
}

void loop() {
    // Process USB tasks at the start of each loop iteration
    tud_task();
    
    unsigned long currentMillis = millis();
    
    // Heartbeat every 10 seconds
    static unsigned long previousMillis = 0;
    const long heartbeatInterval = 10000;
    if (currentMillis - previousMillis >= heartbeatInterval) {
        Serial.println("Heartbeat..");
        previousMillis = currentMillis;
    }
    
    // Update key handler
    if (keyHandler) {
        keyHandler->updateKeys();
        keyHandler->diagnostics();
    }
    
    // Update encoder handler and check encoder changes
    if (encoderHandler) {
        encoderHandler->updateEncoders();
        encoderHandler->diagnostics();
        
        // Example for a single encoder at index 0:
        long delta = encoderHandler->getEncoderChange(0);
        if (delta != 0) {
            // Retrieve HID action for the encoder from your actions JSON
            if (!encoderActionsLoaded) {
                encoderActions = ConfigManager::loadActions("/config/actions.json");
                encoderActionsLoaded = true;
            }
            
            if (encoderActions.find("encoder-1") != encoderActions.end()) {
                ActionConfig encAction = encoderActions["encoder-1"];
                uint8_t report[8] = {0};
                bool wasReportSent = false;
                
                // Detailed debug information about encoder change
                Serial.printf("Encoder delta: %ld\n", delta);
                
                // If turning clockwise (delta negative), use the "clockwise" HID report
                if (delta < 0 && !encAction.clockwise.empty()) {
                    Serial.println("Encoder turned CLOCKWISE");
                    Serial.print("Clockwise HID Report: ");
                    for (const auto& hexStr : encAction.clockwise) {
                        Serial.printf("%s ", hexStr.c_str());
                    }
                    Serial.println();
                    
                    if (HIDHandler::hexReportToBinary(encAction.clockwise, report, 8)) {
                        Serial.print("Converted Clockwise Report: ");
                        for (int i = 0; i < 8; i++) {
                            Serial.printf("%02X ", report[i]);
                        }
                        Serial.println();
                        
                        if (hidHandler) {
                            wasReportSent = hidHandler->sendKeyboardReport(report);
                            Serial.printf("Clockwise Report Sent: %s\n", wasReportSent ? "YES" : "NO");
                        }
                        }
               }
               // If turning counterclockwise (delta positive), use the "counterclockwise" HID report
               else if (delta > 0 && !encAction.counterclockwise.empty()) {
                   Serial.println("Encoder turned COUNTERCLOCKWISE");
                   Serial.print("Counterclockwise HID Report: ");
                   for (const auto& hexStr : encAction.counterclockwise) {
                       Serial.printf("%s ", hexStr.c_str());
                   }
                   Serial.println();
                   
                   if (HIDHandler::hexReportToBinary(encAction.counterclockwise, report, 8)) {
                       Serial.print("Converted Counterclockwise Report: ");
                       for (int i = 0; i < 8; i++) {
                           Serial.printf("%02X ", report[i]);
                       }
                       Serial.println();
                       
                       if (hidHandler) {
                           wasReportSent = hidHandler->sendKeyboardReport(report);
                           Serial.printf("Counterclockwise Report Sent: %s\n", wasReportSent ? "YES" : "NO");
                       }
                   }
               }
           }
       }
   }

   // Update HID handler
   if (hidHandler) {
       hidHandler->update();
   }

   // Check USB status every 5 seconds
   static unsigned long lastUsbCheck = 0;
   if (currentMillis - lastUsbCheck >= 5000) {
       lastUsbCheck = currentMillis;
       
       Serial.println("\n--- USB Status ---");
       Serial.printf("USB Connected: %s\n", tud_mounted() ? "YES" : "NO");
       Serial.printf("USB HID Ready: %s\n", tud_hid_ready() ? "YES" : "NO");
       Serial.printf("USB CDC Ready: %s\n", tud_cdc_connected() ? "YES" : "NO");
       Serial.printf("USB Interface Status: %s\n", (tud_ready()) ? "READY" : "NOT READY");
       Serial.printf("HID Handler Pointer: %s\n", (hidHandler != nullptr) ? "VALID" : "NULL");
       
       // Try to re-initialize HID if needed
       if (!tud_hid_ready() && hidHandler != nullptr) {
           Serial.println("Trying to re-initialize HID...");
           hidHandler->begin();
       }
   }
    
  if (strip) {
      for (int i = 0; i < numLEDs; i++) {
          strip->setPixelColor(i, strip->Color(
              ledConfigs[i].r,
              ledConfigs[i].g,
              ledConfigs[i].b
          ));
      }
      strip->show();
  }

   delay(5); // Small delay to ease CPU usage
}