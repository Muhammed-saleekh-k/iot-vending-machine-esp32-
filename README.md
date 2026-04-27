# IoT Vending Machine Controller — ESP32 + MQTT + HX711

Wireless vending machine firmware with power-cut recovery 
and multi-mode dispensing engine.
Built during traineeship at Opulance Technologies.

---

## Features

- **Universal dispense engine** — Time (ms), Weight (grams), 
  Count, and Energy modes via single MQTT topic
- **Power-cut recovery** — EEPROM job checkpointing every 5s; 
  system detects incomplete dispense on reboot and resumes 
  with remaining target
- **HX711 load cell** — auto-tare before pour for accurate 
  weight-based liquid dispensing
- **Non-blocking state machine** — zero `delay()` calls during 
  active dispensing; MQTT stays responsive throughout
- **4-slot inventory management** — stock validation before 
  dispense, remote refill via MQTT, low-stock alerts
- **MQTT LWT** — JSON state payloads, QoS 1, retained status
- **WiFi provisioning** — SoftAP + REST with machine ID 
  injection for multi-unit deployment
- **Machine-busy guard** — ignores new commands during 
  active dispensing cycle

---

## Hardware Used

| Component | Interface | Purpose |
|---|---|---|
| ESP32 DevKit | — | Main MCU |
| HX711 + Load Cell | GPIO (bit-bang) | Weight measurement |
| Relay Module x4 | GPIO | Motor/pump control |
| Audio Trigger Module | GPIO | Dispense feedback |

---

## MQTT Topic Structure

```
project/vending/{MAC}/status    → Device state (LWT + online payload)
project/vending/{MAC}/register  → Device registration
project/vending/{MAC}/dispense  → Dispense command: slot/mode/target/qty
project/vending/{MAC}/refill    → Refill stock: slot/amount  
project/vending/{MAC}/check     → Stock check: slot number
```

### Dispense Command Format
```
{slot}/{mode}/{target}/{qty}

Examples:
1/T/5000/1   → Slot 1, Time mode, 5000ms, reduce 1 unit
2/W/150/1    → Slot 2, Weight mode, 150g, reduce 1 unit
3/C/1/1      → Slot 3, Count mode
```

---

## Power-Cut Recovery Flow

```
Power cut during dispense
        ↓
ESP32 reboots → checkRecovery() reads EEPROM
        ↓
JOB_ACTIVE_ADDR == 1 → restore slot, mode, remaining target
        ↓
Resume dispense from where it stopped
        ↓
On completion → clear EEPROM job flag
```

---

## Dispense State Machine

```
dispenseItem() called
      ↓
Busy check → Stock check → Set globals → Tare scale (W mode)
      ↓
Motor ON → isDispensing = true → dispenseStartTime = millis()
      ↓
handleDispensing() polls every loop:
  Time mode:   millis() - start >= target?
  Weight mode: scale.get_units() >= target?
      ↓
Target reached → Motor OFF → Update stock → Publish MQTT
```

---

## Build & Flash

1. Install Arduino IDE with ESP32 board support
2. Install libraries: PubSubClient, ArduinoJson, HX711
3. Flash via USB
4. Connect to `OPULANCE-{MAC}` AP on first boot
5. POST to `/vending` with:
   `{"ssid":"wifi","password":"pass","machine_id":"VM001"}`

---

## Status

| Module | Status |
|---|---|
| Firmware | ✅ Complete |
| MQTT tested | ✅ MQTTx verified |
| Load cell calibration | ✅ Working |
| Frontend dashboard | 🔄 In development |

---

## Author

**Muhammed Saleekh K** — Embedded Systems Engineer Trainee  
[LinkedIn](https://www.linkedin.com/in/muhammed-saleekh-k) · 
[GitHub](https://github.com/Muhammed-saleekh-k)
