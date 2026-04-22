# ❄️ AC Control System - Connection Guide

This guide explains how to wire the new **AC Control ESP8266** and the **Tank ESP8266** for the integrated feedback system.

---

## 1. AC Control ESP8266 (New Node)
This node controls the AC using a Relay and an IRLZ44N N-Channel MOSFET as a driver.

### 🧩 Components Needed:
- 1x ESP8266 (NodeMCU or D1 Mini)
- 1x 5V Relay Module
- 1x IRLZ44N MOSFET (Logic Level)
- 1x 10kΩ Resistor (Pull-down)
- 1x 1N4007 Diode (Flyback protection if using raw relay coil)
- 5V Power Supply

### 🔌 Wiring Diagram:
| ESP8266 Pin | Component | Note |
|:--- |:--- |:--- |
| **D1 (GPIO 5)** | MOSFET Gate | Signal to toggle AC |
| **GND** | MOSFET Source | Common Ground |
| **Relay Coil (-)** | MOSFET Drain | Switched Ground |
| **Relay Coil (+)** | 5V VCC | Power for Relay |
| **GND** | 10k Resistor | Connect Gate to GND (Pull-down) |

**AC Side:**
- Connect the **Relay COM** and **Relay NO** (Normally Open) in series with the AC power line (or the AC's manual switch trigger).

---

## 2. Tank Sensor ESP8266 (Updated Node)
This node now includes a visual indicator to show when the AC is running.

### 🧩 Components Needed:
- 1x LED (Any color, Blue recommended for AC)
- 1x 220Ω Resistor

### 🔌 Wiring Diagram:
| ESP8266 Pin | Component | Note |
|:--- |:--- |:--- |
| **D2** | Float Switch | Existing Water Level Sensor |
| **D5 (GPIO 14)** | LED Anode (+) | **NEW: AC Status Indicator** |
| **GND** | LED Cathode (-) | via 220Ω Resistor |

---

## 🚀 Deployment Steps:
1.  **Database**: Run the `master_setup.sql` in your Supabase SQL Editor.
2.  **AC Node**: Upload `ac_control.ino` to the new ESP8266.
3.  **Tank Node**: Upload the updated `tank_sensor.ino` to the existing Tank ESP8266.
4.  **Dashboard**: Refresh your web dashboard to see the new **Climate Control** section.

### 💡 How it works:
- When you turn the AC **ON** via the dashboard or a schedule, the AC ESP activates the relay on **D1**.
- The Tank ESP polls the database every 5 seconds. If it sees the AC is ON, it lights up the LED on **D5**.
- This gives you physical feedback at the tank location even though the AC is controlled by a different device!
