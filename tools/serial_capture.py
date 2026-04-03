#!/usr/bin/env python3
"""
Serial Capture Script for LoRa Mesh Metrics
CSC2106 Group 33

Captures serial output from edge node via USB for offline analysis.
Each line is timestamped for precise timing analysis.

Usage:
    python serial_capture.py --port /dev/ttyUSB0 --output logs/test_run.log
    python serial_capture.py --list  # List available ports
    python serial_capture.py         # Auto-detect port

Dependencies:
    pip install pyserial

Serial Port Examples:
    Linux:   /dev/ttyUSB0, /dev/ttyACM0 (check with: ls /dev/tty*)
    macOS:   /dev/cu.usbserial-*, /dev/cu.SLAB_USBtoUART
    Windows: COM3, COM4, etc.

Linux Permissions (if "Permission denied"):
    sudo usermod -a -G dialout $USER
    # Then log out and back in

Press Ctrl+C to stop capture and save the log file.
"""

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial not installed. Run: pip install pyserial")
    sys.exit(1)


def list_ports():
    """List all available serial ports."""
    ports = serial.tools.list_ports.comports()
    if not ports:
        print("No serial ports found.")
        return []
    
    print("Available serial ports:")
    for i, port in enumerate(ports):
        print(f"  [{i}] {port.device} - {port.description}")
    return ports


def auto_detect_port():
    """Auto-detect likely T-Beam/ESP32 port."""
    ports = serial.tools.list_ports.comports()
    
    # Common USB-serial chip identifiers for ESP32/T-Beam
    keywords = ['CP210', 'CH340', 'FTDI', 'USB Serial', 'Silicon Labs', 'USB-SERIAL']
    
    for port in ports:
        desc = port.description.upper()
        if any(kw.upper() in desc for kw in keywords):
            return port.device
    
    # Fallback: try common port names
    common_ports = ['/dev/ttyUSB0', '/dev/ttyACM0', 'COM3', 'COM4', 'COM5']
    for p in common_ports:
        try:
            s = serial.Serial(p, 115200, timeout=0.1)
            s.close()
            return p
        except:
            pass
    
    return None


def capture_serial(port, output_path, baudrate=115200):
    """Capture serial output to a log file with timestamps."""
    
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    print(f"Opening {port} at {baudrate} baud...")
    
    try:
        ser = serial.Serial(port, baudrate, timeout=1)
    except serial.SerialException as e:
        print(f"ERROR: Could not open {port}: {e}")
        print("Tip: Close Arduino IDE Serial Monitor if it's open.")
        sys.exit(1)
    
    print(f"Capturing to: {output_path}")
    print("Press Ctrl+C to stop capture.\n")
    print("=" * 60)
    
    line_count = 0
    start_time = time.time()
    
    try:
        with open(output_path, 'w', encoding='utf-8') as f:
            # Write header
            f.write(f"# Serial capture started at {datetime.now().isoformat()}\n")
            f.write(f"# Port: {port}, Baudrate: {baudrate}\n")
            f.write("# Format: [HH:MM:SS.mmm] <original line>\n")
            f.write("#" + "=" * 59 + "\n")
            
            while True:
                try:
                    line = ser.readline()
                    if line:
                        # Decode with error handling
                        try:
                            text = line.decode('utf-8', errors='replace').rstrip('\r\n')
                        except:
                            text = str(line)
                        
                        # Add timestamp
                        now = datetime.now()
                        timestamp = now.strftime("%H:%M:%S") + f".{now.microsecond // 1000:03d}"
                        
                        # Write to file
                        f.write(f"[{timestamp}] {text}\n")
                        f.flush()
                        
                        # Echo to console
                        print(f"[{timestamp}] {text}")
                        
                        line_count += 1
                        
                except serial.SerialException as e:
                    print(f"\nSerial error: {e}")
                    break
                    
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        elapsed = time.time() - start_time
        print("\n" + "=" * 60)
        print(f"Capture stopped.")
        print(f"  Lines captured: {line_count}")
        print(f"  Duration: {elapsed:.1f} seconds")
        print(f"  Output saved to: {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Capture serial output from LoRa mesh edge node for metrics analysis.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    python serial_capture.py --port COM3 --output logs/test_run_001.log
    python serial_capture.py --list
    python serial_capture.py  # Auto-detect port
        """
    )
    parser.add_argument('--port', '-p', help='Serial port (e.g., COM3, /dev/ttyUSB0)')
    parser.add_argument('--output', '-o', default='logs/capture.log', 
                        help='Output log file path (default: logs/capture.log)')
    parser.add_argument('--baudrate', '-b', type=int, default=115200,
                        help='Baud rate (default: 115200)')
    parser.add_argument('--list', '-l', action='store_true',
                        help='List available serial ports and exit')
    
    args = parser.parse_args()
    
    if args.list:
        list_ports()
        return
    
    port = args.port
    if not port:
        print("Auto-detecting serial port...")
        port = auto_detect_port()
        if port:
            print(f"  Found: {port}")
        else:
            print("  Could not auto-detect. Available ports:")
            ports = list_ports()
            if ports:
                print("\nRun with --port <port> to specify manually.")
            sys.exit(1)
    
    capture_serial(port, args.output, args.baudrate)


if __name__ == '__main__':
    main()
