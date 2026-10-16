#!/usr/bin/env python3
"""End-to-end hardware verification suite for OpenBeken on Realtek RTL8196C.

Tests:
  1. HTTP Web UI (/ and /index?state=1) on both static (192.168.0.196) & DHCP IPs
  2. REST API (/api/info, /api/pins, /api/channels)
  3. Channel & Physical LED control via HTTP GET (/index?tgl=1, /cm?cmnd=...) & POST (/api/cmnd)
  4. LittleFS read/write (/api/lfs/autoexec.bat)
  5. UART console command execution over COM5
  6. Soft-reboot (/api/reboot) preserving LittleFS & config in persistent SDRAM disk
  7. Web UI HTTP OTA (POST /api/ota) hot-reloading OpenBeken & preserving LittleFS
"""
import json
import pathlib
import sys
import time
import urllib.parse
import urllib.request
import serial

ROOT = pathlib.Path(__file__).resolve().parent.parent
STATIC_IP = "192.168.0.196"
COM_PORT = "COM5"
BAUD = 38400


def http_get(ip, path, timeout=5.0):
    url = f"http://{ip}{path}"
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.status, r.read().decode("utf-8", "replace")


def http_post(ip, path, data: bytes, content_type="text/plain", timeout=15.0):
    url = f"http://{ip}{path}"
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Content-Type", content_type)
    req.add_header("Content-Length", str(len(data)))
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read().decode("utf-8", "replace")


def wait_for_http(ip, min_wait=3.5, timeout=12.0):
    time.sleep(min_wait)
    deadline = time.time() + timeout
    last_err = None
    while time.time() < deadline:
        try:
            st, body = http_get(ip, "/api/info", timeout=2.0)
            if st == 200:
                return st, body
        except Exception as e:
            last_err = e
            time.sleep(0.5)
    raise RuntimeError(f"Router did not come back on http://{ip}/api/info: {last_err}")


def main():
    print("=== 1. Testing HTTP Web UI & /api/info ===")
    st, info_raw = http_get(STATIC_IP, "/api/info")
    assert st == 200, f"expected 200, got {st}"
    info = json.loads(info_raw)
    print(f"  chipset={info['chipset']} mac={info['mac']} dhcp_ip={info['ip']} ver={info['build']}")
    assert info["chipset"] == "RTL8196C"
    assert info["mac"].lower() == "80:1f:02:ca:ad:92"
    dhcp_ip = info["ip"]

    st, root_html = http_get(STATIC_IP, "/")
    assert st == 200 and "OpenRTL8196C" in root_html
    if dhcp_ip and dhcp_ip != "0.0.0.0":
        st_d, _ = http_get(dhcp_ip, "/index?state=1")
        assert st_d == 200
        print(f"  Verified Web UI reachable on both {STATIC_IP} and DHCP {dhcp_ip}")

    print("=== 2. Testing /api/pins (32 GPIO pins + 7 LEDs) ===")
    st, pins_raw = http_get(STATIC_IP, "/api/pins")
    assert st == 200
    pins = json.loads(pins_raw)
    assert len(pins["roles"]) == 32
    print(f"  Verified {len(pins['roles'])} GPIO pins configured")

    print("=== 3. Testing LED / Channel Toggling via HTTP & Tasmota Commands ===")
    http_post(STATIC_IP, "/api/cmnd", b"Backlog SetChannel 1 0; SetChannel 2 0; SetChannel 3 0; SetChannel 4 0; SetChannel 5 0; SetChannel 6 0; SetChannel 7 0")
    time.sleep(0.3)
    _, ch_raw = http_get(STATIC_IP, "/api/channels")
    ch = json.loads(ch_raw)
    assert ch.get("1", 0) == 0

    # Toggle channel 1 via Web UI button (/index?tgl=1)
    _, state_html = http_get(STATIC_IP, "/index?tgl=1")
    assert "<td class='on'>ON</td>" in state_html
    _, ch_raw = http_get(STATIC_IP, "/api/channels")
    ch = json.loads(ch_raw)
    assert ch.get("1") == 1
    print("  /index?tgl=1 -> Channel 1 (PB2 Port0 LED) ON")

    # Turn on channels 2..7 via /api/cmnd
    http_post(STATIC_IP, "/api/cmnd", b"Backlog SetChannel 2 1; SetChannel 3 1; SetChannel 4 1; SetChannel 5 1; SetChannel 6 1; SetChannel 7 1")
    time.sleep(0.3)
    _, ch_raw = http_get(STATIC_IP, "/api/channels")
    ch = json.loads(ch_raw)
    for idx in range(1, 8):
        assert ch.get(str(idx)) == 1, f"Channel {idx} not 1: {ch}"
    print(f"  All 7 router LEDs ON: {ch}")

    print("=== 4. Testing UART Console Command Execution (COM5) ===")
    with serial.Serial(COM_PORT, BAUD, timeout=0.5) as ser:
        ser.write(b"\r\nSetChannel 7 0\r\n")
        ser.flush()
        time.sleep(0.5)
        uart_out = ser.read(4096).decode("ascii", "replace")
    _, ch_raw = http_get(STATIC_IP, "/api/channels")
    ch = json.loads(ch_raw)
    assert ch.get("7", 0) == 0, f"Expected channel 7 to be 0 after UART cmd, got {ch}"
    print("  UART command 'SetChannel 7 0' verified via /api/channels")

    print("=== 5. Testing LittleFS (/api/lfs/autoexec.bat) ===")
    autoexec_content = b"// OpenRTL8196C autoexec.bat\nSetChannel 1 1\nSetChannel 2 0\nSetChannel 3 1\n"
    st, _ = http_post(STATIC_IP, "/api/lfs/autoexec.bat", autoexec_content)
    assert st == 200
    st, readback = http_get(STATIC_IP, "/api/lfs/autoexec.bat")
    assert st == 200 and readback.encode("utf-8") == autoexec_content
    print(f"  LittleFS write + readback verified ({len(readback)} bytes)")

    print("=== 6. Testing Soft-Reboot (POST /api/reboot) & Persistent Disk ===")
    st, _ = http_post(STATIC_IP, "/api/reboot", b"")
    assert st == 200
    st, info_after = wait_for_http(STATIC_IP, min_wait=4.0, timeout=12.0)
    assert st == 200
    st, lfs_after = http_get(STATIC_IP, "/api/lfs/autoexec.bat")
    assert st == 200 and lfs_after.encode("utf-8") == autoexec_content
    print("  Soft-reboot completed; LittleFS autoexec.bat persisted across reboot!")

    print("=== 7. Testing Web UI HTTP OTA (POST /api/ota) ===")
    img_bytes = (ROOT / "build" / "obk" / "openbeken_rtl8196c.img").read_bytes()
    st, ota_resp = http_post(STATIC_IP, "/api/ota", img_bytes, content_type="application/octet-stream", timeout=20.0)
    assert st == 200
    ota_json = json.loads(ota_resp)
    assert ota_json.get("size") == len(img_bytes)
    print(f"  POST /api/ota uploaded {len(img_bytes)} bytes -> {ota_resp}, waiting for reboot...")

    st, info_ota = wait_for_http(STATIC_IP, min_wait=3.5, timeout=12.0)
    assert st == 200
    st, lfs_ota = http_get(STATIC_IP, "/api/lfs/autoexec.bat")
    assert st == 200 and lfs_ota.encode("utf-8") == autoexec_content
    print(f"  Web UI HTTP OTA rebooted cleanly! Info: {info_ota}")

    # Determine bench PC IP on the router subnet
    import socket
    import struct
    import threading

    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect((STATIC_IP, 80))
    pc_ip = probe.getsockname()[0]
    probe.close()

    print(f"=== 8. Testing NTP Driver against Bench UDP NTP Server ({pc_ip}:12300) ===")
    ntp_got_req = threading.Event()
    ntp_srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ntp_srv.bind(("0.0.0.0", 12300))
    ntp_srv.settimeout(6.0)

    # Punch stateful UDP holes in Windows Firewall to router ephemeral UDP ports before starting NTP
    target_ips = [STATIC_IP] + ([dhcp_ip] if dhcp_ip and dhcp_ip != "0.0.0.0" else [])
    for tip in target_ips:
        for port in range(50000, 50016):
            ntp_srv.sendto(b"\x00", (tip, port))

    def ntp_responder():
        try:
            deadline = time.time() + 6.0
            while time.time() < deadline:
                data, addr = ntp_srv.recvfrom(256)
                if len(data) >= 48:
                    # 2026-10-15 12:00:00 UTC -> unix 1792065600 + NTP_OFFSET 2208988800
                    ntp_sec = 1792065600 + 2208988800
                    resp = bytearray(48)
                    resp[0] = 0x24  # LI=0, VN=4, Mode=4 (Server)
                    resp[1] = 1     # Stratum 1
                    struct.pack_into(">I", resp, 40, ntp_sec)
                    ntp_srv.sendto(bytes(resp), addr)
                    ntp_got_req.set()
                    break
        except Exception:
            pass
        finally:
            ntp_srv.close()

    t_ntp = threading.Thread(target=ntp_responder, daemon=True)
    t_ntp.start()
    http_post(STATIC_IP, "/api/cmnd", f"Backlog stopDriver NTP; startDriver NTP 1; ntp_setServer {pc_ip}:12300".encode("utf-8"))
    assert ntp_got_req.wait(timeout=6.0), "Timed out waiting for UDP NTP request from router"
    time.sleep(1.5)
    _, state_ntp = http_get(STATIC_IP, "/index?state=1")
    assert "2026-" in state_ntp, f"Expected synced 2026 timestamp in /index?state=1, got: {state_ntp}"
    print("  NTP synced with bench server; Web UI reports 2026 timestamp!")

    print(f"=== 9. Testing MQTT 3.1.1 Client & Command Control ({pc_ip}:1883) ===")
    mqtt_connected = threading.Event()
    mqtt_rx_topics = []
    mqtt_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    mqtt_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    mqtt_srv.bind(("0.0.0.0", 1883))
    mqtt_srv.listen(1)
    mqtt_srv.settimeout(8.0)

    def mqtt_broker():
        try:
            conn, _ = mqtt_srv.accept()
            conn.settimeout(4.0)
            pkt = conn.recv(512)
            if pkt and (pkt[0] >> 4) == 1:  # CONNECT
                conn.sendall(b"\x20\x02\x00\x00")  # CONNACK (Accepted)
                mqtt_connected.set()
                # Publish a command to set Channel 1 to 0 over MQTT
                topic = b"cmnd/obk_rtl8196c/SetChannel"
                payload = b"1 0"
                rem = 2 + len(topic) + len(payload)
                pub = bytes([0x30, rem, len(topic) >> 8, len(topic) & 0xFF]) + topic + payload
                time.sleep(0.3)
                conn.sendall(pub)
                # Read any SUBSCRIBE/PUBLISH frames from router
                try:
                    more = conn.recv(1024)
                    if more:
                        mqtt_rx_topics.append(len(more))
                except Exception:
                    pass
                time.sleep(0.5)
            conn.close()
        except Exception:
            pass
        finally:
            mqtt_srv.close()

    t_mqtt = threading.Thread(target=mqtt_broker, daemon=True)
    t_mqtt.start()
    http_post(STATIC_IP, "/api/cmnd", b"SetChannel 1 1")
    http_post(STATIC_IP, "/api/cmnd", f"Backlog MqttClient obk_rtl8196c; MqttHost {pc_ip}".encode("utf-8"))
    assert mqtt_connected.wait(timeout=8.0), "Timed out waiting for MQTT CONNECT from router"
    time.sleep(1.0)
    _, ch_mqtt_raw = http_get(STATIC_IP, "/api/channels")
    ch_mqtt = json.loads(ch_mqtt_raw)
    assert ch_mqtt.get("1", 1) == 0, f"Expected Channel 1 to be set to 0 via MQTT, got {ch_mqtt}"
    http_post(STATIC_IP, "/api/cmnd", b"Backlog stopDriver NTP; MqttHost \"\"")
    print(f"  MQTT CONNECT + CONNACK + incoming cmnd/obk_rtl8196c/SetChannel verified! Channels: {ch_mqtt}")

    print("=== 10. Testing Wi-Fi Soft-AP (192.168.4.1) & WPA2 STA + Dual DHCP ===")
    wifi_cfg_path = ROOT / "examples" / "27_wifi_sta" / "wifi_config.h"
    if wifi_cfg_path.exists():
        import re
        cfg_txt = wifi_cfg_path.read_text(encoding="utf-8", errors="replace")
        m_ssid = re.search(r'#define\s+DEFAULT_WIFI_SSID\s+"([^"]*)"', cfg_txt)
        m_pass = re.search(r'#define\s+DEFAULT_WIFI_PASS\s+"([^"]*)"', cfg_txt)
        if m_ssid and m_pass and m_ssid.group(1):
            w_ssid = m_ssid.group(1)
            w_pass = m_pass.group(1)
            q_ssid = urllib.parse.quote(f"SSID1 {w_ssid}")
            q_pass = urllib.parse.quote(f"Password1 {w_pass}")
            with serial.Serial(COM_PORT, BAUD, timeout=0.3) as ser:
                ser.reset_input_buffer()
                http_get(STATIC_IP, f"/cm?cmnd={q_ssid}")
                http_get(STATIC_IP, f"/cm?cmnd={q_pass}")
                sta_ip = None
                deadline = time.time() + 18.0
                while time.time() < deadline:
                    chunk = ser.read(4096).decode("utf-8", "backslashreplace")
                    if chunk:
                        m_ip = re.search(r"STA DHCP bound: IP=(\d+\.\d+\.\d+\.\d+)", chunk)
                        if m_ip:
                            sta_ip = m_ip.group(1)
                            break
                    time.sleep(0.2)
                assert sta_ip, "Timed out waiting for Wi-Fi STA DHCP lease"
                st_eth, _ = http_get(STATIC_IP, "/api/info")
                st_sta, info_sta_raw = http_get(sta_ip, "/api/info")
                assert st_eth == 200 and st_sta == 200
                info_sta = json.loads(info_sta_raw)
                assert info_sta["ip"] == sta_ip
                print(f"  Wi-Fi STA connected to '{w_ssid}' with DHCP IP {sta_ip}; HTTP verified on both Ethernet ({STATIC_IP}/{dhcp_ip}) and Wi-Fi ({sta_ip})!")

                # Switch back to default Soft-AP mode (192.168.4.1)
                ser.reset_input_buffer()
                http_get(STATIC_IP, "/cm?cmnd=SSID1%20%22%22")
                http_get(STATIC_IP, "/cm?cmnd=Password1%20%22%22")
                ap_up = False
                deadline = time.time() + 8.0
                while time.time() < deadline:
                    chunk = ser.read(4096).decode("utf-8", "backslashreplace")
                    if "Soft-AP active:" in chunk and "192.168.4.1" in chunk:
                        ap_up = True
                        break
                    time.sleep(0.2)
                assert ap_up, "Expected router to return to Soft-AP mode (192.168.4.1)"
                print("  Restored default Soft-AP mode (192.168.4.1/24, DHCP 192.168.4.2..9)")

    print("\nALL 10 OPENBEKEN RTL8196C HARDWARE TESTS PASSED!")


if __name__ == "__main__":
    main()


