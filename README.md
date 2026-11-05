# Smart Automated Fish Feeding System

A smart aquarium fish feeder built on the **Arduino Mega 2560** and written entirely in **register-level AVR C**, with no Arduino functions. It feeds fish on a daily schedule, monitors temperature and humidity, and shows live status on a cycling LCD.

**Live simulation:** [Open in Wokwi](https://wokwi.com/projects/462742763509847041)

## Features

- Scheduled feeding at 08:00, 12:00 and 18:00 using a DS1307 RTC
- Manual feeding with a push button
- Temperature and humidity monitoring with a DHT-22
- 16x2 I2C LCD that cycles through four screens every 3 seconds: time/date, temperature/humidity, next feeding, system status
- RGB LED for state indication and a Timer2-driven buzzer for alerts
- Servo-driven feeder mechanism using Timer1 Fast PWM
- Error state with a red LED if the DHT-22 fails

## Hardware

- Arduino Mega 2560
- DS1307 RTC
- DHT-22 sensor
- Servo motor
- 16x2 LCD with I2C backpack
- RGB LED (common cathode) with 220 Ω resistors
- Buzzer
- Push button
- Potentiometer

## Pin Mapping

| Peripheral | Arduino Pin |
|---|---|
| LCD and RTC (I2C) | 20 (SDA), 21 (SCL) |
| DHT-22 | 2 |
| Push button | 3 |
| RGB LED (R / G / B) | 6 / 7 / 5 |
| Buzzer | 8 |
| Servo | 12 |
| Potentiometer | A0 |

## Run in Wokwi

1. Open the [Wokwi project](https://wokwi.com/projects/462742763509847041), or create a new **Arduino Mega** project.
2. Copy `sketch.ino` and `diagram.json` from this repository into it.
3. Start the simulation.

No external libraries are required. The RTC is set to 08:00:00 at startup for demo purposes.

## Author

Bora Yıldız

## License

MIT
