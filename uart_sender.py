import serial
import time
import sys
import os

def send_firmware(port, filename):
    if not os.path.exists(filename):
        print(f"Error: File {filename} not found.")
        sys.exit(1)

    with open(filename, "rb") as f:
        data = f.read()
    
    try:
        ser = serial.Serial(port, 115200, timeout=1)
    except Exception as e:
        print(f"Error opening serial port {port}: {e}")
        sys.exit(1)
    
    file_size = len(data)
    print(f"File size: {file_size} bytes")
    
    # Calculate simple checksum (sum of bytes)
    checksum = sum(data) & 0xFFFFFFFF
    
    # Send START
    cmd = f"FW:S:{file_size:08X}\n"
    ser.write(cmd.encode())
    print("Sent START")
    time.sleep(2) # Wait for initial flash erase (Mass Erase / Sector Erase takes time)
    
    # Send DATA chunks
    chunk_size = 64
    for i in range(0, file_size, chunk_size):
        chunk = data[i:i+chunk_size]
        hex_data = chunk.hex().upper()
        cmd = f"FW:D:{i:08X}:{len(chunk):04X}:{hex_data}\n"
        
        ser.write(cmd.encode())
        print(f"Sent chunk offset 0x{i:05X} len {len(chunk)}")
        
        # Add slight delay to avoid UDP congestion and TSCH queue overflow
        # 0.15 seconds allows the coordinator to send the packet over TSCH properly with shared period 11
        time.sleep(0.15)
        
    # Send VERIFY
    time.sleep(1)
    cmd = f"FW:V:{checksum:08X}\n"
    ser.write(cmd.encode())
    print(f"Sent VERIFY, expected checksum 0x{checksum:08X}")
    
    ser.close()
    print("Done!")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python uart_sender.py <serial_port> <firmware.bin>")
        sys.exit(1)
    send_firmware(sys.argv[1], sys.argv[2])
