import serial
import time
import sys
import os

def crc16_add(b: int, acc: int) -> int:
    acc ^= b
    acc &= 0xFFFF
    acc = ((acc >> 8) | (acc << 8)) & 0xFFFF
    acc ^= ((acc & 0xFF00) << 4) & 0xFFFF
    acc ^= ((acc >> 8) >> 4) & 0xFFFF
    acc ^= ((acc & 0xFF00) >> 5) & 0xFFFF
    return acc

def crc16_data(data: bytes, acc: int = 0) -> int:
    for b in data:
        acc = crc16_add(b, acc)
    return acc

def send_command_with_ack(ser, cmd, expected_ack="FW:ACK", timeout=0.5):
    """
    Sends a command to the serial port and waits for the expected acknowledgement.
    If timeout is reached and no expected ACK is received, it retries up to 5 times.
    If cmd is empty, it acts as a passive listener for the expected_ack with a single timeout.
    """
    if not cmd:
        # Passive wait (e.g. for page distribution complete PAGE_OK)
        start_time = time.time()
        while time.time() - start_time < timeout:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line:
                with open("coordinator.log", "a", encoding="utf-8") as f_log:
                    f_log.write(line + "\n")
                if line == expected_ack:
                    return True
        return False

    # Active send and wait with retries
    for attempt in range(5):
        ser.write(cmd.encode())
        start_time = time.time()
        while time.time() - start_time < timeout:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line:
                with open("coordinator.log", "a", encoding="utf-8") as f_log:
                    f_log.write(line + "\n")
                if line == expected_ack:
                    return True
        print(f"Timeout waiting for {expected_ack}, retrying (attempt {attempt+1}/5)...")
        time.sleep(0.1) # Small delay before retry
    return False

def handle_error_and_listen(ser, error_msg):
    print(f"\n{error_msg}")
    print("Entering passive listening mode to monitor logs. Press Ctrl+C to exit.")
    try:
        while True:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line:
                print(f"[Serial] {line}")
                with open("coordinator.log", "a", encoding="utf-8") as f_log:
                    f_log.write(line + "\n")
            else:
                time.sleep(0.01)
    except KeyboardInterrupt:
        print("\nExiting.")
    finally:
        ser.close()
        sys.exit(1)

def send_firmware(port, filename):
    if not os.path.exists(filename):
        print(f"Error: File {filename} not found.")
        sys.exit(1)

    with open(filename, "rb") as f:
        data = f.read()
    
    try:
        # Setting a small read timeout (e.g. 100ms) for responsive readline loops
        ser = serial.Serial(port, 115200, timeout=0.1)
    except Exception as e:
        print(f"Error opening serial port {port}: {e}")
        sys.exit(1)
    
    file_size = len(data)
    print(f"File size: {file_size} bytes")
    
    # Calculate CRC-16 CCITT (matching Contiki-NG crc16_data)
    checksum = crc16_data(data, 0)
    
    # Clear the log file at the start of the session
    with open("coordinator.log", "w", encoding="utf-8") as f_log:
        f_log.write(f"=== New OTA Session: {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n")

    # Send START
    print("Sending START...")
    cmd = f"FW:S:{file_size:08X}\n"
    if not send_command_with_ack(ser, cmd, "FW:ACK", timeout=1.5):
        handle_error_and_listen(ser, "Error: Coordinator did not ACK START command.")
    print("START ACK received.")

    # Wait for sensor nodes to complete flash erase and resynchronize on period 61,
    # before they transition to period 11.
    print("Waiting 5 seconds for nodes to complete sector 0 erase and stabilize TSCH...")
    time.sleep(5.0)

    # Send DATA chunks in pages
    page_size = 4096
    chunk_size = 32
    
    for page_offset in range(0, file_size, page_size):
        current_page_size = min(page_size, file_size - page_offset)
        print(f"\n--- Sending page at offset 0x{page_offset:05X} (size: {current_page_size} bytes) ---")
        
        # Send chunks of this page
        for i in range(0, current_page_size, chunk_size):
            chunk_offset = page_offset + i
            chunk = data[chunk_offset:chunk_offset + min(chunk_size, current_page_size - i)]
            hex_data = chunk.hex().upper()
            cmd = f"FW:D:{chunk_offset:08X}:{len(chunk):04X}:{hex_data}\n"
            
            # Send chunk and wait for ACK (timeout 0.5s is safe, retries handled automatically)
            if not send_command_with_ack(ser, cmd, "FW:ACK", timeout=0.5):
                handle_error_and_listen(ser, f"Error: Failed to transmit chunk at offset 0x{chunk_offset:05X} after 5 attempts.")
            print(f"Sent chunk offset 0x{chunk_offset:05X} len {len(chunk)}")
            
        print("Page chunks sent. Waiting for network distribution (FW:PAGE_OK)...")
        
        # Wait for page distribution confirmation (timeout 120s is safe to accommodate coordinator timeouts/retries and 500ms pacing)
        if not send_command_with_ack(ser, "", "FW:PAGE_OK", timeout=120.0):
            handle_error_and_listen(ser, "Error: Coordinator failed to distribute page within 120 seconds.")
        print("Page distribution complete.")
        
    # Send VERIFY
    print("Sending VERIFY...")
    cmd = f"FW:V:{checksum:08X}\n"
    if not send_command_with_ack(ser, cmd, "FW:ACK", timeout=2.0):
        handle_error_and_listen(ser, "Error: Coordinator did not ACK VERIFY command.")
    print("VERIFY ACK received.")
    
    # Wait for verification to complete and write logs to file
    print("Waiting for verification results...")
    start_time = time.time()
    while time.time() - start_time < 5:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if line:
            with open("coordinator.log", "a", encoding="utf-8") as f_log:
                f_log.write(line + "\n")
            
    ser.close()
    print("Done!")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python uart_sender.py <serial_port> <firmware.bin>")
        sys.exit(1)
    send_firmware(sys.argv[1], sys.argv[2])
