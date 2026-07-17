# SpinDoctor

**Unplanned downtime from equipment failure costs manufacturers billions of dollars a year, and most of it is preventable if you catch the failure early enough.**

Industrial predictive maintenance solves this with vibration sensors and on-device AI, but building and testing that pipeline usually requires access to real production machinery. SpinDoctor proves out the same core approach end to end, real-time fault classification running entirely on embedded hardware, using a table fan as an accessible, safe stand-in for real rotating machinery.

It detects three states in real time: **healthy**, **blade imbalance**, and **obstruction**, classified entirely on-device with no cloud dependency for detection itself, then reports the result to a live dashboard and gets a plain-language explanation from an LLM.

<table>
<tr>
<td rowspan="2" width="38%" align="center">
<img src="Docs/Images/Hardware.png" alt="SpinDoctor physical hardware setup" width="100%"/>
<br/><sub>Physical rig: STM32F407 + LIS3DSH + DHT11 mounted on the test fan</sub>
</td>
<td width="62%" align="center">
<img src="Docs/Images/Dashboard.png" alt="SpinDoctor live dashboard" width="100%"/>
<br/><sub>Live dashboard with real-time digital twin</sub>
</td>
</tr>
<tr>
<td align="center">

![STM32](https://img.shields.io/badge/MCU-STM32F407-03234B?style=flat-square&logo=stmicroelectronics&logoColor=white)
![FreeRTOS](https://img.shields.io/badge/RTOS-FreeRTOS-00979D?style=flat-square)
![TinyML](https://img.shields.io/badge/AI-NanoEdge%20TinyML-FF6F00?style=flat-square)
![ESP32](https://img.shields.io/badge/Gateway-ESP32-E7352C?style=flat-square&logo=espressif&logoColor=white)
![Gemini](https://img.shields.io/badge/LLM-Gemini%20API-8E75B2?style=flat-square&logo=googlegemini&logoColor=white)
![Pages](https://img.shields.io/badge/Dashboard-GitHub%20Pages-222222?style=flat-square&logo=github&logoColor=white)

<br/>

**<a href="https://shrutik-kapatel.github.io/SpinDoctor/">Live Dashboard</a> · <a href="TECHNICAL.md">Full Technical Writeup</a>**

</td>
</tr>
</table>

---
## What It Does

A microcontroller mounted directly on a fan reads vibration (3-axis accelerometer) and temperature 400 times a second, runs a machine learning model on that data **on the chip itself**, and instantly knows if the fan is:

- 🟢 **Healthy** - running normally
- 🟡 **Imbalanced** - a blade is off-balance, an early warning sign before real damage
- 🔴 **Obstructed** - something is blocking or dragging on the fan

No internet connection is needed for this detection to work, it happens entirely on the embedded chip in real time. Once a fault is detected, a second chip (ESP32) picks up the result over WiFi, asks an AI language model (Gemini) to explain what's happening in plain English, logs it, and streams it to a live web dashboard with a real-time animated "digital twin" of the fan.

```
Fan vibrates → Chip senses & classifies → Result explained by AI → Shown live on dashboard
```
