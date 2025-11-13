// --- Pin Definitions ---
#define ECHO_PIN 4          // Pin ECHO sensor ultrasonik
#define TRIG_PIN 5          // Pin TRIG sensor ultrasonik
#define SOLENOID_PIN 6      // Pin untuk relay solenoid
#define LIMIT_SWITCH_PIN 7  // Pin untuk limit switch
#define BUZZER_PIN 8        // Pin untuk buzzer

// --- Constants ---
#define ULTRASONIC_THRESHOLD_CM 27
#define REALTIME_DELAY 200      // Delay untuk update saat box terbuka
#define IDLE_FEEDBACK_DELAY 500 // Delay untuk update feedback saat box terkunci

// --- States untuk Finite State Machine (FSM) ---
enum State {
  IDLE_LOCKED,
  BOX_OPEN_AWAITING_PICKUP,
  ITEM_TAKEN_AWAITING_CLOSURE
};
State currentState = IDLE_LOCKED;

// --- Global Flags ---
boolean itemTaken = false;
boolean buzzerActive = false;
boolean sendData = true;      // "Saklar" utama untuk pengiriman data
boolean stopRequested = false; // "Bendera penanda" permintaan stop dari Python

// --- Fungsi untuk Membaca Sensor Ultrasonik --- 
float readUltrasonicDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000); 
  return (duration == 0) ? 999.0 : (duration * 0.0343) / 2;
}

// --- Fungsi untuk mengirim feedback terstruktur ke Tkinter ---
void sendFeedbackToTkinter() {
  String ultrasonicStatus_tk;
  String limitStatus_tk;
  String buzzerStatus_tk;
  String systemState_tk;
  boolean currentLimitStatusLow = (digitalRead(LIMIT_SWITCH_PIN) == LOW);

  if (currentState == IDLE_LOCKED) {
    float distance_locked_cm = readUltrasonicDistance();
    ultrasonicStatus_tk = (distance_locked_cm <= ULTRASONIC_THRESHOLD_CM) ? "Ada Barang" : "Tidak Ada Barang";
  } else {
    ultrasonicStatus_tk = itemTaken ? "Tidak Ada Barang" : "Ada Barang";
  }
  
  limitStatus_tk = currentLimitStatusLow ? "Rapat" : "Tidak Rapat";
  buzzerStatus_tk = buzzerActive ? "Hidup" : "Mati";

  switch (currentState) {
    case IDLE_LOCKED: systemState_tk = "Terkunci"; break;
    case BOX_OPEN_AWAITING_PICKUP: systemState_tk = "CekBarang"; break;
    case ITEM_TAKEN_AWAITING_CLOSURE: systemState_tk = "Tunggu Tutup"; break;
    default: systemState_tk = "Error"; break;
  }

  String feedback = "US:" + ultrasonicStatus_tk + ";LM:" + limitStatus_tk + ";BZ:" + buzzerStatus_tk + ";ST:" + systemState_tk;
  Serial.println(feedback);
}

// --- Setup ---
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

// --- Loop Utama ---
void loop() {
  // Selalu dengarkan perintah dari komputer
  if (Serial.available() > 0) {
    String data = Serial.readStringUntil('\n');
    data.trim();
    
    if (data == "true" && currentState == IDLE_LOCKED) {
      sendData = true;      // Saklar ON, mulai kirim data
      stopRequested = false; // Turunkan bendera stop untuk siklus baru
      digitalWrite(SOLENOID_PIN, LOW); 
      currentState = BOX_OPEN_AWAITING_PICKUP;
      itemTaken = false; 
      digitalWrite(BUZZER_PIN, LOW); 
      buzzerActive = false;
    } 
    else if (data == "false") {
      stopRequested = true; // Terima permintaan stop, naikkan bendera
    }
  }

  // --- LOGIKA UTAMA HANYA BERJALAN JIKA "SAKLAR" ON ---
  if (sendData) {
    float distance_cm; 
    boolean isBoxClosedProperly; 

    switch (currentState) {
      case IDLE_LOCKED:
        // Cek apakah ada permintaan berhenti dan kondisi sudah aman (terkunci)
        if (stopRequested) {
          sendData = false;      // Matikan saklar pengiriman data
          stopRequested = false; // Turunkan kembali benderanya
        }
        
        isBoxClosedProperly = (digitalRead(LIMIT_SWITCH_PIN) == LOW);
        if (itemTaken && !isBoxClosedProperly) {
          digitalWrite(SOLENOID_PIN, LOW);
          buzzerActive = true;
          digitalWrite(BUZZER_PIN, HIGH);
          currentState = ITEM_TAKEN_AWAITING_CLOSURE;
        } else { 
          digitalWrite(SOLENOID_PIN, HIGH);
          digitalWrite(BUZZER_PIN, LOW);
          buzzerActive = false;
        }
        sendFeedbackToTkinter();
        delay(IDLE_FEEDBACK_DELAY);
        break;

      case BOX_OPEN_AWAITING_PICKUP:
        digitalWrite(BUZZER_PIN, LOW); 
        buzzerActive = false;
        
        isBoxClosedProperly = (digitalRead(LIMIT_SWITCH_PIN) == LOW);
        if (isBoxClosedProperly) {
            currentState = IDLE_LOCKED;
            digitalWrite(SOLENOID_PIN, HIGH);
            sendFeedbackToTkinter();
            break;
        }

        distance_cm = readUltrasonicDistance();
        if (distance_cm > ULTRASONIC_THRESHOLD_CM) {
          itemTaken = true; 
          currentState = ITEM_TAKEN_AWAITING_CLOSURE;
        }
        
        sendFeedbackToTkinter();
        delay(REALTIME_DELAY); 
        break;

      case ITEM_TAKEN_AWAITING_CLOSURE:
        distance_cm = readUltrasonicDistance();
        if (distance_cm <= ULTRASONIC_THRESHOLD_CM) {
          itemTaken = false; 
          currentState = BOX_OPEN_AWAITING_PICKUP; 
          digitalWrite(BUZZER_PIN, LOW); 
          buzzerActive = false;
          sendFeedbackToTkinter(); 
          break; 
        }
        
        if (itemTaken) {
          isBoxClosedProperly = (digitalRead(LIMIT_SWITCH_PIN) == LOW);
          buzzerActive = !isBoxClosedProperly;
          digitalWrite(BUZZER_PIN, buzzerActive ? HIGH : LOW);

          if (isBoxClosedProperly) {
            digitalWrite(SOLENOID_PIN, HIGH); 
            currentState = IDLE_LOCKED;
            digitalWrite(BUZZER_PIN, LOW); 
            buzzerActive = false;
          }
        }
        
        sendFeedbackToTkinter();
        delay(REALTIME_DELAY);
        break;
    }
  } // --- Akhir dari blok "if (sendData)" ---
}