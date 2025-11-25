// --- Pin Definitions & Constants (SAMA SEPERTI SEBELUMNYA) ---
#define ECHO_PIN 4
#define TRIG_PIN 5
#define SOLENOID_PIN 6
#define LIMIT_SWITCH_PIN 7
#define BUZZER_PIN 8
#define ULTRASONIC_THRESHOLD_CM 27
#define REALTIME_DELAY 200
#define IDLE_FEEDBACK_DELAY 500

// --- States ---
enum State {
  IDLE_LOCKED,
  BOX_OPEN_AWAITING_ACTION,     // Nama diganti sedikit biar umum
  ACTION_DONE_AWAITING_CLOSURE  // Nama diganti sedikit biar umum
};
State currentState = IDLE_LOCKED;

// --- Mode Operasi (BARU) ---
enum OperationMode {
  MODE_NONE,
  MODE_SENDER,   // Mode Pengirim (Menaruh Paket)
  MODE_RECEIVER  // Mode Penerima (Mengambil Paket)
};
OperationMode currentMode = MODE_NONE;

// --- Global Flags ---
boolean actionCompleted = false; // Ganti 'itemTaken' jadi lebih umum
boolean buzzerActive = false;
boolean sendData = true;
boolean stopRequested = false;
unsigned long unlockTime = 0;           // Mencatat waktu kapan kunci dibuka
const long IGNORE_SENSOR_DURATION = 3000; // Durasi mengabaikan sensor (5 detik)
boolean isSensorIgnored = false;        // Status apakah sedang mengabaikan sensor

// --- Fungsi Ultrasonic (SAMA) ---
float readUltrasonicDistance() {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  return (duration == 0) ? 999.0 : (duration * 0.0343) / 2;
}

// --- Fungsi Feedback (DIUPDATE DIKIT) ---
void sendFeedbackToTkinter() {
  String ultrasonicStatus_tk;
  // Baca dulu sensor aslinya
  boolean currentLimitStatusLow = (digitalRead(LIMIT_SWITCH_PIN) == LOW);
  String limitStatus_tk;

  // Logika Manipulasi (Grace Period)
  if (isSensorIgnored && currentLimitStatusLow) {
      limitStatus_tk = "Tidak Rapat"; // Bohong: Bilang terbuka padahal tertutup
  } else {
      limitStatus_tk = currentLimitStatusLow ? "Rapat" : "Tidak Rapat"; // Jujur
  }
  String buzzerStatus_tk = buzzerActive ? "Hidup" : "Mati";
  String systemState_tk;

  // Logika status barang disesuaikan agar tidak bingung
  float dist = readUltrasonicDistance();
  if (dist <= ULTRASONIC_THRESHOLD_CM) {
     ultrasonicStatus_tk = "Ada Barang";
  } else {
     ultrasonicStatus_tk = "Tidak Ada Barang";
  }

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
    
    // --- LOGIKA PARSING PERINTAH BARU ---
    // Format dari Python harus: "sender:TIMESTAMP" atau "receiver:TIMESTAMP"
    
    if (currentState == IDLE_LOCKED) {
       if (data.startsWith("sender:")) {
          currentMode = MODE_SENDER;
          String timestamp = data.substring(7); 
          
          digitalWrite(SOLENOID_PIN, LOW);
          unlockTime = millis();
          isSensorIgnored = true;
          
          Serial.println("LATENCY_LOG:" + timestamp);
          sendData = true; stopRequested = false;
          currentState = BOX_OPEN_AWAITING_ACTION;
          actionCompleted = false;
          digitalWrite(BUZZER_PIN, LOW); buzzerActive = false;
       }
       else if (data.startsWith("receiver:")) { // Dulu ini "true:"
          currentMode = MODE_RECEIVER;
          String timestamp = data.substring(9);
          
          digitalWrite(SOLENOID_PIN, LOW);
          unlockTime = millis();
          isSensorIgnored = true;
          
          Serial.println("LATENCY_LOG:" + timestamp);
          sendData = true; stopRequested = false;
          currentState = BOX_OPEN_AWAITING_ACTION;
          actionCompleted = false;
          digitalWrite(BUZZER_PIN, LOW); buzzerActive = false;
       }
    }
    
    if (data == "false") stopRequested = true;
  }

  if (sendData) {
    float distance_cm;
    boolean isBoxClosedProperly;

    switch (currentState) {
      case IDLE_LOCKED:
        if (stopRequested) { sendData = false; stopRequested = false; }
        sendFeedbackToTkinter();
        delay(IDLE_FEEDBACK_DELAY);
        break;

      case BOX_OPEN_AWAITING_ACTION: // Dulu AWAITING_PICKUP
        digitalWrite(BUZZER_PIN, LOW); buzzerActive = false;
        isBoxClosedProperly = (digitalRead(LIMIT_SWITCH_PIN) == LOW);
        
        // --- [LOGIKA PINTAR: HYBRID] ---
        if (isSensorIgnored) {
            unsigned long timePassed = millis() - unlockTime;

            // 1. Kalau waktu 5 detik habis, ya matikan timer
            if (timePassed > IGNORE_SENSOR_DURATION) {
                isSensorIgnored = false;
            }
            // 2. TAPI.. Kalau baru lewat 1 detik DAN pintu dibuka manual oleh User...
            // (Artinya bukan getaran solenoid lagi, tapi beneran tangan manusia)
            else if (timePassed > 1000 && !isBoxClosedProperly) {
                isSensorIgnored = false; // Matikan timer SEKARANG juga!
            }
        }
        
        // Fitur Safety: Kalau ditutup tapi belum ada transaksi, kunci lagi aja
        if (isBoxClosedProperly && !isSensorIgnored) {
            currentState = IDLE_LOCKED;
            digitalWrite(SOLENOID_PIN, HIGH);
            currentMode = MODE_NONE; // Reset mode
            sendFeedbackToTkinter();
            break;
        }

        distance_cm = readUltrasonicDistance();
        
        // --- INI BAGIAN KRUSIAL: LOGIKA CABANG DUA ---
        if (currentMode == MODE_RECEIVER) {
            // Logika Penerima: Tunggu barang HILANG (Jarak Jauh)
            if (distance_cm > ULTRASONIC_THRESHOLD_CM) {
               actionCompleted = true;
               currentState = ACTION_DONE_AWAITING_CLOSURE;
               Serial.println("LOG:ITEM_TAKEN"); 
            }
        } 
        else if (currentMode == MODE_SENDER) {
            // Logika Pengirim: Tunggu barang MUNCUL (Jarak Dekat)
            if (distance_cm <= ULTRASONIC_THRESHOLD_CM) {
               actionCompleted = true;
               currentState = ACTION_DONE_AWAITING_CLOSURE;
               Serial.println("LOG:ITEM_PLACED"); // Log baru
            }
        }
        
        sendFeedbackToTkinter();
        delay(REALTIME_DELAY);
        break;

      case ACTION_DONE_AWAITING_CLOSURE:
        // Logika ini berlaku umum untuk Sender maupun Receiver
        // Intinya: Transaksi sudah dianggap sah, sekarang tutup pintunya!
        
        isBoxClosedProperly = (digitalRead(LIMIT_SWITCH_PIN) == LOW);
        buzzerActive = !isBoxClosedProperly; // Bunyi kalau gak rapet
        digitalWrite(BUZZER_PIN, buzzerActive ? HIGH : LOW);

        if (isBoxClosedProperly) {
           digitalWrite(SOLENOID_PIN, HIGH);
           currentState = IDLE_LOCKED;
           currentMode = MODE_NONE; // Reset mode
           digitalWrite(BUZZER_PIN, LOW); 
           buzzerActive = false;
           Serial.println("LOG:BOX_CLOSED");
        }
        
        sendFeedbackToTkinter();
        delay(REALTIME_DELAY);
        break;
    }
  }
}
