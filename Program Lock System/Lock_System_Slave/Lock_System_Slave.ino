// ==========================================
// FIRMWARE DELIVERY ROBOT (FINAL FIX - STABLE SENSOR)
// ==========================================

// --- Pin Definitions ---
#define ECHO_PIN 4
#define TRIG_PIN 5
#define SOLENOID_PIN 6
#define LIMIT_SWITCH_PIN 7
#define BUZZER_PIN 8

// --- Constants ---
#define ULTRASONIC_THRESHOLD_CM 27
#define REALTIME_DELAY 200      
#define IDLE_FEEDBACK_DELAY 500 

// --- States ---
enum State {
  IDLE_LOCKED,
  BOX_OPEN_AWAITING_ACTION,     
  ACTION_DONE_AWAITING_CLOSURE  
};
State currentState = IDLE_LOCKED;

// --- Mode Operasi ---
enum OperationMode {
  MODE_NONE,
  MODE_SENDER,   
  MODE_RECEIVER  
};
OperationMode currentMode = MODE_NONE;

// --- Global Flags ---
boolean actionCompleted = false; 
boolean buzzerActive = false;
boolean sendData = true;      
boolean stopRequested = false; 

// --- Variabel Logika Grace Period (Pintu Jatuh) ---
unsigned long unlockTime = 0;             
const long IGNORE_SENSOR_DURATION = 5000; // 5 detik abaikan sensor awal
boolean isSensorIgnored = false;          

// --- Variabel Delay Buzzer (Sopan Santun) ---
unsigned long actionDoneTime = 0;         
const long BUZZER_START_DELAY = 3000;     // Delay 3 detik sebelum buzzer bunyi
// ----------------------------------------

// --- Fungsi Ultrasonic Stabil (Filter Rata-Rata) ---
// [PERBAIKAN]: Fungsi ini ditaruh di LUAR, bukan di dalam fungsi lain.
float readUltrasonicDistance() {
  float totalDistance = 0;
  int validReadings = 0;

  // Ambil 5 sampel biar stabil (Anti-Goyang)
  for (int i = 0; i < 5; i++) {
      digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
      digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
      digitalWrite(TRIG_PIN, LOW);
      
      long duration = pulseIn(ECHO_PIN, HIGH, 30000); 
      float distance = (duration == 0) ? 0 : (duration * 0.0343) / 2;

      if (distance > 2.0) { // Filter data 0 atau noise
          totalDistance += distance;
          validReadings++;
      }
      delay(5); 
  }

  if (validReadings == 0) return 999.0;
  return totalDistance / validReadings;
}

// --- Fungsi Feedback ---
void sendFeedbackToTkinter() {
  String ultrasonicStatus_tk;
  boolean currentLimitStatusLow = (digitalRead(LIMIT_SWITCH_PIN) == LOW);
  String limitStatus_tk;

  // Logika Manipulasi (Grace Period)
  if (isSensorIgnored && currentLimitStatusLow) {
      limitStatus_tk = "Tidak Rapat"; 
  } else {
      limitStatus_tk = currentLimitStatusLow ? "Rapat" : "Tidak Rapat"; 
  }

  // Logika Ultrasonik
  float dist = readUltrasonicDistance();
  if (dist <= ULTRASONIC_THRESHOLD_CM) {
     ultrasonicStatus_tk = "Ada Barang";
  } else {
     ultrasonicStatus_tk = "Tidak Ada Barang";
  }
  
  String buzzerStatus_tk = buzzerActive ? "Hidup" : "Mati";
  String systemState_tk;

  switch (currentState) {
    case IDLE_LOCKED: systemState_tk = "Terkunci"; break;
    case BOX_OPEN_AWAITING_ACTION: 
         systemState_tk = (currentMode == MODE_SENDER) ? "Tunggu Barang Masuk" : "Tunggu Barang Ambil"; 
         break;
    case ACTION_DONE_AWAITING_CLOSURE: systemState_tk = "Tunggu Tutup"; break;
    default: systemState_tk = "Error"; break;
  }

  String feedback = "US:" + ultrasonicStatus_tk + ";LM:" + limitStatus_tk + ";BZ:" + buzzerStatus_tk + ";ST:" + systemState_tk;
  Serial.println(feedback);
}

void setup() {
  Serial.begin(115200);
  pinMode(SOLENOID_PIN, OUTPUT);
  pinMode(LIMIT_SWITCH_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  digitalWrite(SOLENOID_PIN, HIGH);
  digitalWrite(BUZZER_PIN, LOW);
}

void loop() {
  if (Serial.available() > 0) {
    String data = Serial.readStringUntil('\n');
    data.trim();
    
    if (currentState == IDLE_LOCKED) {
       if (data.startsWith("sender:")) {
          currentMode = MODE_SENDER;
          String timestamp = data.substring(7); 
          digitalWrite(SOLENOID_PIN, LOW);
          Serial.println("LATENCY_LOG:" + timestamp);
          
          unlockTime = millis(); isSensorIgnored = true; 
          
          sendData = true; stopRequested = false;
          currentState = BOX_OPEN_AWAITING_ACTION;
          actionCompleted = false;
          digitalWrite(BUZZER_PIN, LOW); buzzerActive = false;
       }
       else if (data.startsWith("receiver:")) { 
          currentMode = MODE_RECEIVER;
          String timestamp = data.substring(9);
          digitalWrite(SOLENOID_PIN, LOW);
          Serial.println("LATENCY_LOG:" + timestamp);
          
          unlockTime = millis(); isSensorIgnored = true; 
          
          sendData = true; stopRequested = false;
          currentState = BOX_OPEN_AWAITING_ACTION;
          actionCompleted = false;
          digitalWrite(BUZZER_PIN, LOW); buzzerActive = false;
       }
    }
    if (data == "false") stopRequested = true;
  }

  if (sendData) {
    // Pakai pembacaan Stabil
    float distance_cm = readUltrasonicDistance(); 
    
    // Debugging (Opsional, boleh dihapus kalau menuuh-menuhin serial)
    // Serial.print("Jarak: "); Serial.println(distance_cm);

    boolean isBoxClosedProperly; 

    switch (currentState) {
      case IDLE_LOCKED:
        if (stopRequested) { sendData = false; stopRequested = false; }
        sendFeedbackToTkinter();
        delay(IDLE_FEEDBACK_DELAY);
        break;

      case BOX_OPEN_AWAITING_ACTION:
        digitalWrite(BUZZER_PIN, LOW); buzzerActive = false;
        isBoxClosedProperly = (digitalRead(LIMIT_SWITCH_PIN) == LOW);
        
        // --- LOGIKA PINTAR: HYBRID ---
        if (isSensorIgnored) {
            unsigned long timePassed = millis() - unlockTime;
            if (timePassed > IGNORE_SENSOR_DURATION) {
                isSensorIgnored = false;
            }
            else if (timePassed > 1000 && !isBoxClosedProperly) {
                isSensorIgnored = false; 
            }
        }

        if (isBoxClosedProperly && !isSensorIgnored) {
            currentState = IDLE_LOCKED;
            digitalWrite(SOLENOID_PIN, HIGH);
            currentMode = MODE_NONE; 
            sendFeedbackToTkinter();
            break;
        }
        
        // --- LOGIKA TRANSAKSI ---
        if (currentMode == MODE_RECEIVER) {
            // [Fix]: Tambah !isBoxClosedProperly
            if (distance_cm > ULTRASONIC_THRESHOLD_CM && !isBoxClosedProperly) {
               actionCompleted = true;
               currentState = ACTION_DONE_AWAITING_CLOSURE;
               actionDoneTime = millis(); 
               Serial.println("LOG:ITEM_TAKEN"); 
            }
        } 
        else if (currentMode == MODE_SENDER) {
            // [Fix]: Tambah !isBoxClosedProperly biar Sender juga aman
            if (distance_cm <= ULTRASONIC_THRESHOLD_CM && !isBoxClosedProperly) {
               actionCompleted = true;
               currentState = ACTION_DONE_AWAITING_CLOSURE;
               actionDoneTime = millis(); 
               Serial.println("LOG:ITEM_PLACED"); 
            }
        }
        
        sendFeedbackToTkinter();
        delay(REALTIME_DELAY); 
        break;

      case ACTION_DONE_AWAITING_CLOSURE:
        isSensorIgnored = false; 
        isBoxClosedProperly = (digitalRead(LIMIT_SWITCH_PIN) == LOW);
        
        // --- LOGIKA DELAY BUZZER ---
        if (!isBoxClosedProperly) {
            if (millis() - actionDoneTime > BUZZER_START_DELAY) {
               buzzerActive = true; 
            } else {
               buzzerActive = false; 
            }
        } else {
            buzzerActive = false; 
        }
        
        digitalWrite(BUZZER_PIN, buzzerActive ? HIGH : LOW);

        if (isBoxClosedProperly) {
           digitalWrite(SOLENOID_PIN, HIGH);
           currentState = IDLE_LOCKED;
           currentMode = MODE_NONE; 
           actionDoneTime = 0;
           digitalWrite(BUZZER_PIN, LOW); buzzerActive = false;
           Serial.println("LOG:BOX_CLOSED");
        }
        
        sendFeedbackToTkinter();
        delay(REALTIME_DELAY);
        break;
    }
  }
}