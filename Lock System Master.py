import tkinter as tk
from tkinter import messagebox , filedialog
import csv
import os
import shutil
import random
import qrcode
from PIL import Image, ImageTk
import firebase_admin
from firebase_admin import credentials, db, exceptions as firebase_exceptions
import serial
import time
import threading
from queue import Queue, Empty
from datetime import datetime

# --- "Papan Catatan" untuk update GUI yang aman ---
update_queue = Queue()

# --- Konstanta Konfigurasi ---
FIREBASE_CERT_PATH = 'qr-baru-firebase-adminsdk-ofxtt-0a2f2c31b8.json'
FIREBASE_DB_URL = 'https://qr-baru-default-rtdb.asia-southeast1.firebasedatabase.app/'
LOGO_PATH = "LOGO DELIVERY ROBOT.PNG"
SERIAL_PORT_1 = 'COM15'
SERIAL_PORT_2 = 'COM11'
SERIAL_BAUDRATE = 115200
QR_REFRESH_INTERVAL_MS = 30000
SERIAL_CHECK_INTERVAL_MS = 200
QUEUE_CHECK_INTERVAL_MS = 100
LOG_FILENAME = 'Data_Latensi_Wifi 2.csv'

# --- Variabel Global & Status Koneksi ---
firebase_connected = False
serial_connected_1 = False
serial_connected_2 = False
esp32_1 = None
esp32_2 = None
main_app_window = None
container_window = None
active_container_state = {}

# --- Inisialisasi Firebase & Serial ---
try:
    if not firebase_admin._apps:
        cred = credentials.Certificate(FIREBASE_CERT_PATH)
        firebase_admin.initialize_app(cred, {'databaseURL': FIREBASE_DB_URL})
    firebase_connected = True
    print("[INIT] Firebase berhasil diinisialisasi.")
except Exception as e:
    root = tk.Tk(); root.withdraw(); messagebox.showerror("Firebase Error", f"Tidak dapat menginisialisasi Firebase: {e}"); root.destroy(); exit()

try:
    esp32_1 = serial.Serial(SERIAL_PORT_1, SERIAL_BAUDRATE, timeout=1)
    serial_connected_1 = True
    print(f"[INIT] Berhasil terhubung ke Container 1 di {SERIAL_PORT_1}.")
except serial.SerialException as e:
    print(f"[PERINGATAN] Gagal terhubung ke {SERIAL_PORT_1}. Error: {e}")

try:
    esp32_2 = serial.Serial(SERIAL_PORT_2, SERIAL_BAUDRATE, timeout=1)
    serial_connected_2 = True
    print(f"[INIT] Berhasil terhubung ke Container 2 di {SERIAL_PORT_2}.")
except serial.SerialException as e:
    print(f"[PERINGATAN] Gagal terhubung ke {SERIAL_PORT_2}. Error: {e}")

# --- Fungsi Inti ---

def firebase_event_handler(event):
    """Satu handler pintar untuk semua event dari Firebase. Dijalankan oleh Pekerja Latar Belakang."""
    global active_container_state, serial_connected_1, serial_connected_2
    
    if event.path != "/" or event.data is None: return

    print(f"[FIREBASE] Data diterima dari listener: {event.data}")

    # Pastikan data yang masuk adalah dictionary dan container sedang aktif
    if isinstance(event.data, dict) and 'qr_data' in event.data and active_container_state:
        scanned_qr = str(event.data.get('qr_data'))
        timestamp_awal_pc = int(time.time() * 1000)

        try:
            # --- FIX #1: Bandingkan hanya 'scanned_qr' dengan nomor acak ---
            if scanned_qr == str(active_container_state.get('random_number')):
                print("[FIREBASE] Verifikasi BERHASIL! Menaruh tugas di 'Papan Catatan'.")
                active_container_state['scan_timestamp'] = timestamp_awal_pc
                write_log(active_container_state.get('name'), "VERIFICATION_SUCCESS", f"Scan QR untuk nomor {scanned_qr} berhasil.")
                update_queue.put(("update_status", "Verified! ✅", "green"))
                
                serial_port_active = active_container_state.get('serial_object')
                container_name = active_container_state.get('name')
                is_currently_connected = serial_connected_1 if container_name == "Container 1" else serial_connected_2
                
                if is_currently_connected and serial_port_active:
                    try:
                        command_timestamp = int(time.time() * 1000)
                        # --- MODIFIKASI MODE SENDER/RECEIVER ---
                        current_mode = active_container_state.get('mode', 'RECEIVER') # Default Receiver
                        
                        if current_mode == 'SENDER':
                            command_str = f"sender:{command_timestamp}\n"
                            print(f"[LOGIC] Mode PENGIRIM terdeteksi. Mengirim: {command_str.strip()}")
                        else:
                            command_str = f"receiver:{command_timestamp}\n"
                            print(f"[LOGIC] Mode PENERIMA terdeteksi. Mengirim: {command_str.strip()}")

                        serial_port_active.write(command_str.encode('utf-8'))
                        print(f"[SERIAL] Perintah 'true' dengan timestamp {command_timestamp} dikirim ke {serial_port_active.port}")

                    except serial.SerialException as e:
                        print(f"[ERROR] Gagal menulis ke serial {serial_port_active.port}: {e}")
                        update_queue.put(("update_status", "Serial Write Error!", "red"))
                        if container_name == "Container 1": serial_connected_1 = False
                        else: serial_connected_2 = False
                else:
                    update_queue.put(("update_status", "Verified! Serial Not Connected.", "orange"))
            else:
                print("[FIREBASE] Verifikasi GAGAL! Nomor QR tidak cocok.")
        except Exception as e:
            print(f"[ERROR] Terjadi error di thread Firebase handler: {e}")

# --- FUNGSI BARU UNTUK MENULIS LOG ---
def write_log(container_name, event_type, detail_message):
    """Menulis catatan aktivitas ke Firebase dengan timestamp."""
    if not firebase_connected:
        print(f"[LOGGING] Gagal: Firebase tidak terhubung.")
        return
    try:
        # Menggunakan format timestamp dengan milidetik
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        log_entry = {
            'timestamp': timestamp,
            'container': container_name,
            'event': event_type,
            'detail': detail_message
        }
        db.reference('/logs').push(log_entry)
        print(f"[LOG] Event: {event_type} - Detail: {detail_message}")
    except Exception as e:
        print(f"[ERROR] Gagal menulis log ke Firebase: {e}")

def generate_qr_and_firebase_update():
    global active_container_state
    if not container_window or not container_window.winfo_exists() or not active_container_state: return
    random_number = random.randint(100000, 999999)
    active_container_state['random_number'] = random_number
    qr = qrcode.QRCode(box_size=10, border=4); qr.add_data(str(random_number)); qr.make(fit=True)
    tk_qr_image = ImageTk.PhotoImage(qr.make_image(fill_color="black", back_color="white"))
    if active_container_state.get('labels'):
        active_container_state['labels']['qr'].config(image=tk_qr_image)
        active_container_state['labels']['qr'].image = tk_qr_image
        status_label = active_container_state['labels']['status']
        status_label.config(text="Waiting For Scan...", fg="#FF7D2D")
    if firebase_connected:
        container_name = active_container_state.get('name', 'Unknown')
        firebase_path = 'QR_Generator_1' if container_name == "Container 1" else 'QR_Generator_2'
        try: 
            db.reference('/Data').update({firebase_path: random_number})
            print(f"[QR] Nomor baru {random_number} dibuat untuk {container_name}.")
            write_log(container_name, "QR_GENERATED", f"Nomor {random_number} dibuat.")
        except firebase_exceptions.FirebaseError as e:
            if 'status_label' in locals() and status_label.winfo_exists(): status_label.config(text="Firebase Update Error!", fg="red")
    if container_window and container_window.winfo_exists():
        active_container_state['jobs']['qr'] = container_window.after(QR_REFRESH_INTERVAL_MS, generate_qr_and_firebase_update)

def parse_arduino_feedback(line):
    data = {};
    try:
        for part in line.strip().split(';'):
            if ':' in part: key, value = part.split(':', 1); data[key.strip()] = value.strip()
    except Exception as e: print(f"Error parsing: '{line}' - {e}")
    return data

def update_arduino_labels_from_feedback(feedback_data):
    if not feedback_data or not container_window or not container_window.winfo_exists() or not active_container_state: return
    labels = active_container_state.get('labels', {})
    if labels:
        labels['ultrasonic'].config(text=f"Barang    : {feedback_data.get('US', '--')}")
        labels['limit'].config(text=f"Box       : {feedback_data.get('LM', '--')}")
        labels['buzzer'].config(text=f"Buzzer    : {feedback_data.get('BZ', '--')}")
        labels['sys_state'].config(text=f"Status    : {feedback_data.get('ST', '--')}")

def append_to_csv_log(scan_time_ms, lock_time_ms, latency):
    """Menambahkan satu baris data ke file log CSV permanen."""
    file_exists = os.path.exists(LOG_FILENAME)
    
    # Format waktu yang benar: jam:menit:detik.milidetik (3 digit)
    scan_time_str = datetime.fromtimestamp(scan_time_ms / 1000).strftime('%H:%M:%S.%f')[:-3]
    lock_time_str = datetime.fromtimestamp(lock_time_ms / 1000).strftime('%H:%M:%S.%f')[:-3]
    
    with open(LOG_FILENAME, 'a', newline='', encoding='utf-8') as file:
        writer = csv.writer(file)
        if not file_exists or os.path.getsize(LOG_FILENAME) == 0:
            writer.writerow(['Timestamp Awal', 'Timestamp Akhir', 'Latensi (ms)']) # Header
        writer.writerow([scan_time_str, lock_time_str, f"{latency} ms"])

def check_serial_data_periodically():
    """Loop permanen yang memeriksa kedua port serial jika ada jendela kontainer yang aktif."""
    # --- PERBAIKAN: Beritahu Python untuk menggunakan variabel global ---
    global serial_connected_1, serial_connected_2
    
    if active_container_state and container_window and container_window.winfo_exists():
        container_name = active_container_state.get('name')
        port_to_check = esp32_1 if container_name == "Container 1" else esp32_2
        is_connected_now = serial_connected_1 if container_name == "Container 1" else serial_connected_2

        if is_connected_now and port_to_check and port_to_check.in_waiting > 0:
            try:
                serial_line = port_to_check.readline().decode('utf-8', errors='replace').strip()
                if serial_line:
                    # Cek dulu apakah ini pesan LATENCY_LOG, LOG, atau data status
                    if serial_line.startswith("LATENCY_LOG:"):
                        lock_open_timestamp = int(serial_line.split(":", 1)[1]) # <- Ambil Timestamp B
                        scan_timestamp = active_container_state.get('scan_timestamp') # <- Ambil Timestamp A yang disimpan   
                        if scan_timestamp:
                            latency_ms = lock_open_timestamp - scan_timestamp
                            print(f"[LATENCY] Latensi dari Scan hingga Loker Terbuka: {latency_ms} ms")
                            write_log(container_name, "LATENCY_MEASURED", f"{latency_ms} ms")
                            append_to_csv_log(scan_timestamp, lock_open_timestamp, latency_ms)

                    if serial_line.startswith("LOG:"):
                        event_detail = serial_line.split(":", 1)[1]
                        if event_detail == "ITEM_TAKEN":
                            write_log(container_name, "ITEM_PICKED_UP", "Sensor ultrasonik mendeteksi barang diambil.")
                        elif event_detail == "BOX_CLOSED":
                            write_log(container_name, "BOX_CLOSED", "Sensor magnetik mendeteksi pintu ditutup.")
                    else:
                        # Jika bukan pesan log, proses seperti biasa
                        print(f"[SERIAL] Data diterima dari {port_to_check.port}: {serial_line}")
                        update_arduino_labels_from_feedback(parse_arduino_feedback(serial_line))
            except serial.SerialException as e:
                # Sekarang Python tahu kita mengubah variabel global
                if container_name == "Container 1": serial_connected_1 = False
                else: serial_connected_2 = False
                print(f"[ERROR] Gagal membaca serial {port_to_check.port}: {e}.")
    
    # Jadwalkan pengecekan berikutnya secara permanen
    main_app_window.after(SERIAL_CHECK_INTERVAL_MS, check_serial_data_periodically)

def export_to_csv(event=None):
    """Menyalin file log permanen ke lokasi yang dipilih pengguna."""
    if not os.path.exists(LOG_FILENAME) or os.path.getsize(LOG_FILENAME) == 0:
        messagebox.showinfo("Info", "File log 'log_latensi.csv' masih kosong.")
        return

    filepath = filedialog.asksaveasfilename(
        defaultextension=".csv",
        filetypes=[("CSV files", "*.csv"), ("All files", "*.*")],
        title="Simpan Salinan Laporan Latensi",
        initialfile="laporan_latensi.csv"
    )
    if not filepath: return

    try:
        shutil.copy(LOG_FILENAME, filepath)
        messagebox.showinfo("Sukses", f"Data berhasil diekspor ke:\n{filepath}")
    except Exception as e:
        messagebox.showerror("Error", f"Gagal mengekspor data: {e}")

def close_container_window():
    """Fungsi ini sekarang mengirim sinyal 'stop' ke Arduino sebelum menutup."""
    global container_window, active_container_state
    if container_window and container_window.winfo_exists():
        container_name = active_container_state.get('name', 'kontainer tidak dikenal')
        print(f"[GUI] Menutup jendela untuk {container_name}...")
        
        # --- TAMBAHAN BARU: Kirim sinyal 'stop' ---
        serial_port_active = active_container_state.get('serial_object')
        if serial_port_active and serial_port_active.is_open:
            try:
                serial_port_active.write(b'false\n')
                print(f"[SERIAL] Sinyal 'stop' (false) dikirim ke {serial_port_active.port}")
            except serial.SerialException as e:
                print(f"[ERROR] Gagal mengirim sinyal 'stop': {e}")
        # --- AKHIR TAMBAHAN ---

        for job_id in active_container_state.get('jobs', {}).values():
            if job_id: container_window.after_cancel(job_id)
        container_window.destroy()
        
    container_window = None
    active_container_state = {}
    main_app_window.deiconify()

def open_container_window(container_name, mode):
    """Fungsi ini sekarang mengirim sinyal 'start' saat jendela dibuka."""
    global container_window, active_container_state
    if container_window and container_window.winfo_exists(): close_container_window()
    
    print(f"[GUI] Membuka jendela utama untuk {container_name}...")
    main_app_window.withdraw()
    
    serial_obj = esp32_1 if container_name == "Container 1" else esp32_2
    active_container_state = {
        'name': container_name, 'serial_object': serial_obj,
        'mode': mode,
        'random_number': None, 'labels': {}, 'jobs': {}
    }
    
    # --- TAMBAHAN PENTING DI SINI ---
    # Kirim sinyal 'start' ke Arduino yang sesuai agar ia mulai mengirim data
    if serial_obj and serial_obj.is_open:
        try:
            serial_obj.write(b'start\n')
            print(f"[SERIAL] Sinyal 'start' dikirim ke {serial_obj.port}")
        except serial.SerialException as e:
            print(f"[ERROR] Gagal mengirim sinyal 'start': {e}")
    # --- AKHIR TAMBAHAN ---
    
    container_window = tk.Toplevel(main_app_window)
    container_window.title(f"{container_name} - QR & Status")
    container_window.attributes('-fullscreen', True)
    container_window.configure(bg="#292929")
    container_window.protocol("WM_DELETE_WINDOW", close_container_window)
    
    tk.Label(container_window, text=f"Scan for {container_name}", font=("Roboto", 30, "bold"), fg="#FF7D2D", bg="#292929").pack(pady=20)
    content_frame = tk.Frame(container_window, bg="#292929"); content_frame.pack(expand=True, fill="both", pady=10, padx=20)
    qr_area_frame = tk.Frame(content_frame, bg="#292929"); qr_area_frame.pack(side="left", expand=True, fill="y", padx=10, pady=100)
    active_container_state['labels']['qr'] = tk.Label(qr_area_frame, bg="#292929"); active_container_state['labels']['qr'].pack(pady=10)
    active_container_state['labels']['status'] = tk.Label(qr_area_frame, text="Initializing...", font=("Roboto", 18, "bold"), fg="#FF7D2D", bg="#292929"); active_container_state['labels']['status'].pack(padx=10, pady=10)
    sensor_status_frame = tk.Frame(content_frame, bg="#292929"); sensor_status_frame.pack(side="right", expand=True, fill="y", padx=10, pady=150)
    tk.Label(sensor_status_frame, text=f"Status Box - {container_name}:", font=("Roboto", 20, "bold"), fg="#FF7D2D", bg="#292929").pack(pady=(0, 15))
    label_font = ("Roboto", 14); label_fg = "#FFFFFF"; label_bg = "#292929"
    active_container_state['labels']['ultrasonic'] = tk.Label(sensor_status_frame, text="Barang    : --", font=label_font, fg=label_fg, bg=label_bg, anchor="w"); active_container_state['labels']['ultrasonic'].pack(pady=3, fill="x")
    active_container_state['labels']['limit'] = tk.Label(sensor_status_frame, text="Box       : --", font=label_font, fg=label_fg, bg=label_bg, anchor="w"); active_container_state['labels']['limit'].pack(pady=3, fill="x")
    active_container_state['labels']['buzzer'] = tk.Label(sensor_status_frame, text="Buzzer    : --", font=label_font, fg=label_fg, bg=label_bg, anchor="w"); active_container_state['labels']['buzzer'].pack(pady=3, fill="x")
    active_container_state['labels']['sys_state'] = tk.Label(sensor_status_frame, text="Status    : --", font=(label_font[0], label_font[1], "bold"), fg=label_fg, bg=label_bg, anchor="w"); active_container_state['labels']['sys_state'].pack(pady=8, fill="x")
    tk.Button(container_window, text="Kembali ke Menu Utama", command=close_container_window, font=("Roboto", 14, "bold"), fg="#FFFFFF", bg="#FF7D2D", width=20, height=2).pack(pady=20, side="bottom")

    generate_qr_and_firebase_update()

def show_logo_and_switch(location, container_name, mode):
    print(f"[GUI] Menampilkan logo untuk pengantaran ke {location}...")
    logo_window = tk.Toplevel(main_app_window); logo_window.attributes('-fullscreen', True); logo_window.configure(bg="#292929")
    try:
        pil_image = Image.open(LOGO_PATH).resize((400, 400), Image.Resampling.LANCZOS)
        tk_logo_image = ImageTk.PhotoImage(pil_image)
        logo_display_label = tk.Label(logo_window, image=tk_logo_image, bg="#292929"); logo_display_label.image = tk_logo_image
        logo_display_label.place(relx=0.5, rely=0.5, anchor="center")
        logo_window.after(3000, lambda: [logo_window.destroy(), open_container_window(container_name, mode)])
    except Exception as e:
        messagebox.showerror("Logo Error", f"Could not load logo: {e}"); logo_window.destroy(); open_container_window(container_name, mode)

def open_mode_selection_window(location, container_name):
    """Jendela baru untuk memilih peran User (Sender/Receiver)"""
    main_app_window.withdraw()
    mode_window = tk.Toplevel(main_app_window)
    mode_window.attributes('-fullscreen', True)
    mode_window.configure(bg="#292929")
    
    tk.Label(mode_window, text=f"Akses {container_name}", font=("Roboto", 24, "bold"), fg="#FF7D2D", bg="#292929").pack(pady=(80, 20))

    btn_frame = tk.Frame(mode_window, bg="#292929")
    btn_frame.pack(pady=40, expand=True)

    # Gaya Tombol
    btn_sender = {"font": ("Roboto", 16, "bold"), "fg": "white", "bg": "#FF7D2D", "width": 15, "height": 3} # Hijau
    btn_receiver = {"font": ("Roboto", 16, "bold"), "fg": "white", "bg": "#FF7D2D", "width": 15, "height": 3} # Biru

    # Tombol SENDER
    tk.Button(btn_frame, text="Sender\n(Put Item)",
              command=lambda: [mode_window.destroy(), show_logo_and_switch(location, container_name, "SENDER")], 
              **btn_sender).pack(side="left", padx=20)

    # Tombol RECEIVER
    tk.Button(btn_frame, text="Receiver\n(Take Item)",
              command=lambda: [mode_window.destroy(), show_logo_and_switch(location, container_name, "RECEIVER")], 
              **btn_receiver).pack(side="left", padx=20)

    # Tombol Batal
    tk.Button(mode_window, text="Batal", command=lambda: [mode_window.destroy(), open_container_selection_window(location)], 
              font=("Roboto", 14), fg="#FFFFFF", bg="#FF7D2D").pack(side="bottom", pady=50)

def open_container_selection_window(location):
    print("[GUI] Membuka jendela pemilihan kontainer.")
    main_app_window.withdraw()
    selection_window = tk.Toplevel(main_app_window); selection_window.attributes('-fullscreen', True); selection_window.configure(bg="#292929")
    def go_back(): selection_window.destroy(); main_app_window.deiconify()
    selection_window.protocol("WM_DELETE_WINDOW", go_back)
    tk.Label(selection_window, text=f"Tujuan: {location}", font=("Roboto", 24, "bold"), fg="#FF7D2D", bg="#292929").pack(pady=(80, 0))
    tk.Label(selection_window, text="Pick Container", font=("Roboto", 18), fg="#FFFFFF", bg="#292929").pack(pady=20)
    btn_frame = tk.Frame(selection_window, bg="#292929"); btn_frame.pack(pady=40, expand=True)
    btn_style = {"font": ("Roboto", 16, "bold"), "fg": "#FFFFFF", "bg": "#FF7D2D", "width": 15, "height": 3}
    tk.Button(btn_frame, text="Container 1", command=lambda: [selection_window.destroy(), open_mode_selection_window(location, "Container 1")], **btn_style).pack(side="left", padx=40)
    tk.Button(btn_frame, text="Container 2", command=lambda: [selection_window.destroy(), open_mode_selection_window(location, "Container 2")], **btn_style).pack(side="left", padx=40)
    tk.Button(selection_window, text="Kembali", command=go_back, font=("Roboto", 14), fg="#FFFFFF", bg="#FF7D2D").pack(side="bottom", pady=50)

def process_queue():
    """Fungsi untuk memproses update GUI dari 'papan catatan' (queue)."""
    try:
        message = update_queue.get_nowait()
        print(f"[QUEUE] Memproses pesan dari 'Papan Catatan': {message}")
        if message[0] == "update_status":
            _, text, color = message
            if container_window and container_window.winfo_exists() and active_container_state.get('labels'):
                active_container_state['labels']['status'].config(text=text, fg=color)
    except Empty: pass
    finally: main_app_window.after(QUEUE_CHECK_INTERVAL_MS, process_queue)

if __name__ == "__main__":
    print("[INIT] Memulai Aplikasi Delivery Robot...")
    main_app_window = tk.Tk()
    main_app_window.title("Delivery Robot"); main_app_window.attributes('-fullscreen', True); main_app_window.configure(bg="#292929")
    
    tk.Label(main_app_window, text="Delivery Robot", font=("Roboto", 40, "bold"), fg="#FF7D2D", bg="#292929").pack(pady=(150, 50))
    location_frame = tk.Frame(main_app_window, bg="#292929"); location_frame.pack(expand=True)
    locations = ["Kantin", "GU", "Tekno", "TA", "RTF"]; loc_btn_style = {"font": ("Roboto", 14, "bold"), "fg": "#FFFFFF", "bg": "#FF7D2D", "width": 12, "height": 3, "relief": "raised", "bd": 3}
    for location in locations:
        tk.Button(location_frame, text=location, command=lambda loc=location: open_container_selection_window(loc), **loc_btn_style).pack(side="left", padx=20, pady=20)
    tk.Label(main_app_window, text="© 2025 Delivery Robot", font=("Roboto", 12), fg="#FFFFFF", bg="#292929").pack(side="bottom", pady=10)

    main_app_window.bind_all("<Control-e>", export_to_csv)

    def on_closing():
        print("[INFO] Aplikasi sedang ditutup...")
        if esp32_1 and esp32_1.is_open: esp32_1.close()
        if esp32_2 and esp32_2.is_open: esp32_2.close()
        main_app_window.destroy()
    main_app_window.protocol("WM_DELETE_WINDOW", on_closing)
    
    # --- Mulai semua proses background permanen DI SINI ---
    if firebase_connected:
        print("[INIT] Memulai listener Firebase permanen di latar belakang...")
        firebase_listen_path = '/Data/QR_Scanner'
        threading.Thread(target=lambda: db.reference(firebase_listen_path).listen(firebase_event_handler), daemon=True).start()

    check_serial_data_periodically()
    process_queue()
    
    main_app_window.mainloop()
    print("[INFO] Aplikasi telah ditutup.")