/*
pins i used:
LCD1602 I2C  -> SDA=20 SCL=21
DS1307 RTC   -> SDA=20 SCL=21  (same bus)
DHT22        -> pin 2
Servo        -> pin 12
Buzzer       -> pin 8
Button       -> pin 3
Potentiometer-> A0
RGB Red      -> pin 6
RGB Green    -> pin 7
RGB Blue     -> pin 5

lcd has 4 screens, changes every 3 sec:
 0 - time/date
 1 - temp + humidity
 2 - next feeding
 3 - status

feeding times: 08:00, 12:00, 18:00
also manual button works
*/

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <util/twi.h>
#include <stdio.h>
#include <string.h>

#define F_CPU 16000000UL

// I2C setup
#define TWI_FREQ    100000UL
#define TWI_BITRATE ((F_CPU / TWI_FREQ - 16) / 2)

// LCD backpack address (PCF8574)
#define LCD_ADDR 0x27
#define LCD_BL   0x08
#define LCD_EN   0x04
#define LCD_RS   0x01

// RTC address
#define RTC_ADDR 0x68

// DHT22 on pin 2 = PE4
#define DHT_DDR  DDRE
#define DHT_PORT PORTE
#define DHT_PIN  PINE
#define DHT_BIT  PE4

// buzzer pin 8 = PH5
#define BUZ_DDR  DDRH
#define BUZ_PORT PORTH
#define BUZ_BIT  PH5

// button pin 3 = PE5
#define BTN_DDR  DDRE
#define BTN_PORT PORTE
#define BTN_PIN  PINE
#define BTN_BIT  PE5

// RGB pins - red and green on H, blue on E
#define RGB_DDR_H  DDRH
#define RGB_PORT_H PORTH
#define RGB_R_BIT  PH3
#define RGB_G_BIT  PH4
#define RGB_DDR_E  DDRE
#define RGB_PORT_E PORTE
#define RGB_B_BIT  PE3

// scheduled feeding times
#define FEED1_H 8
#define FEED1_M 0
#define FEED2_H 12
#define FEED2_M 0
#define FEED3_H 18
#define FEED3_M 0

#define SCREEN_COUNT    4
#define SCREEN_DURATION 3000

// servo pulse range in microseconds
#define SERVO_MIN_US 700
#define SERVO_MAX_US 2300

// millisecond timer variable
volatile uint32_t g_ms = 0;

// time struct for rtc
typedef struct {
    uint8_t sec;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
} RTC_t;

RTC_t g_time;

float   g_temp   = 0.0f;
float   g_humid  = 0.0f;
uint8_t g_dht_ok = 0;

// system state
typedef enum {
    STANDBY = 0,
    FEEDING,
    ERROR
} State_t;

State_t g_state = STANDBY;

uint8_t  g_screen    = 0;
uint32_t g_screen_ms = 0;

// track last fed minute so we dont trigger twice in same minute
uint8_t g_last_fed_min = 255;


// Timer0 interrupt, counts milliseconds
ISR(TIMER0_COMPA_vect) {
    g_ms++;
}

void timer0_init(void) {
    // CTC mode, prescaler 64, OCR0A=249 => 1ms tick at 16MHz
    TCCR0A = (1 << WGM01);
    OCR0A  = 249;
    TIMSK0 = (1 << OCIE0A);
    TCCR0B = (1 << CS01) | (1 << CS00);
}

uint32_t get_ms(void) {
    uint32_t t;
    cli();
    t = g_ms;
    sei();
    return t;
}

void wait_ms(uint32_t ms) {
    uint32_t start = get_ms();
    while ((get_ms() - start) < ms);
}


// I2C 
void i2c_init(void) {
    TWSR = 0x00;
    TWBR = (uint8_t)TWI_BITRATE;
    TWCR = (1 << TWEN);
}

static void i2c_start(void) {
    TWCR = (1 << TWSTA) | (1 << TWEN) | (1 << TWINT);
    while (!(TWCR & (1 << TWINT)));
}

static void i2c_stop(void) {
    TWCR = (1 << TWSTO) | (1 << TWEN) | (1 << TWINT);
    while (TWCR & (1 << TWSTO));
}

static void i2c_write(uint8_t data) {
    TWDR = data;
    TWCR = (1 << TWEN) | (1 << TWINT);
    while (!(TWCR & (1 << TWINT)));
}

static uint8_t i2c_read_ack(void) {
    TWCR = (1 << TWEN) | (1 << TWINT) | (1 << TWEA);
    while (!(TWCR & (1 << TWINT)));
    return TWDR;
}

static uint8_t i2c_read_nack(void) {
    TWCR = (1 << TWEN) | (1 << TWINT);
    while (!(TWCR & (1 << TWINT)));
    return TWDR;
}


// LCD over I2C
static void lcd_pulse_en(uint8_t d) {
    i2c_start();
    i2c_write(LCD_ADDR << 1);
    i2c_write(d | LCD_EN | LCD_BL);
    i2c_stop();
    _delay_us(1);
    i2c_start();
    i2c_write(LCD_ADDR << 1);
    i2c_write((d & ~LCD_EN) | LCD_BL);
    i2c_stop();
    _delay_us(50);
}

static void lcd_write_nibble(uint8_t nibble, uint8_t mode) {
    lcd_pulse_en((nibble & 0xF0) | mode);
}

static void lcd_send(uint8_t val, uint8_t mode) {
    lcd_write_nibble(val & 0xF0, mode);
    lcd_write_nibble((val << 4) & 0xF0, mode);
}

void lcd_cmd(uint8_t cmd) {
    lcd_send(cmd, 0x00);
    _delay_ms(2);
}

void lcd_char(char c) {
    lcd_send((uint8_t)c, LCD_RS);
}

void lcd_str(const char *s) {
    while (*s) lcd_char(*s++);
}

void lcd_goto(uint8_t row, uint8_t col) {
    lcd_cmd((row == 0 ? 0x80 : 0xC0) + col);
}

void lcd_clear(void) {
    lcd_cmd(0x01);
    _delay_ms(2);
}

void lcd_init(void) {
    _delay_ms(50);
    // send 0x30 three times then switch to 4-bit mode
    lcd_write_nibble(0x30, 0); _delay_ms(5);
    lcd_write_nibble(0x30, 0); _delay_ms(1);
    lcd_write_nibble(0x30, 0); _delay_ms(1);
    lcd_write_nibble(0x20, 0);
    lcd_cmd(0x28); // 2 lines, 5x8
    lcd_cmd(0x08); // display off
    lcd_cmd(0x01); // clear
    _delay_ms(2);
    lcd_cmd(0x06); // auto increment
    lcd_cmd(0x0C); // display on, no cursor
}

// print two digit number with leading zero
static void lcd_2d(uint8_t n) {
    lcd_char('0' + n / 10);
    lcd_char('0' + n % 10);
}

// print float with 1 decimal place
static void lcd_float(float f) {
    char buf[8];
    int16_t w = (int16_t)f;
    int16_t d = (int16_t)((f - (float)w) * 10.0f);
    if (d < 0) d = -d;
    snprintf(buf, sizeof(buf), "%d.%d", w, d);
    lcd_str(buf);
}


// DS1307 RTC 
static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

void rtc_read(RTC_t *t) {
    i2c_start();
    i2c_write(RTC_ADDR << 1);
    i2c_write(0x00);
    i2c_stop();
    i2c_start();
    i2c_write((RTC_ADDR << 1) | 0x01);
    t->sec    = bcd2dec(i2c_read_ack() & 0x7F);
    t->minute = bcd2dec(i2c_read_ack());
    t->hour   = bcd2dec(i2c_read_ack() & 0x3F);
    i2c_read_ack(); // skip day-of-week, dont need
    t->day    = bcd2dec(i2c_read_ack());
    t->month  = bcd2dec(i2c_read_ack());
    t->year   = bcd2dec(i2c_read_nack());
    i2c_stop();
}

void rtc_set(uint8_t h, uint8_t m, uint8_t s,
             uint8_t day, uint8_t mon, uint8_t yr) {
    i2c_start();
    i2c_write(RTC_ADDR << 1);
    i2c_write(0x00);
    i2c_write(dec2bcd(s));
    i2c_write(dec2bcd(m));
    i2c_write(dec2bcd(h));
    i2c_write(0x01);
    i2c_write(dec2bcd(day));
    i2c_write(dec2bcd(mon));
    i2c_write(dec2bcd(yr));
    i2c_stop();
}


// DHT22 bit-bang
uint8_t dht22_read(float *temp, float *humid) {
    uint8_t  data[5] = {0};
    uint16_t tmo;

    // host start signal: pull low >1ms then release
    DHT_DDR  |=  (1 << DHT_BIT);
    DHT_PORT &= ~(1 << DHT_BIT);
    _delay_ms(2);
    DHT_PORT |=  (1 << DHT_BIT);
    _delay_us(30);
    DHT_DDR  &= ~(1 << DHT_BIT);
    DHT_PORT |=  (1 << DHT_BIT);

    // wait for sensor ack
    tmo = 10000;
    while ( (DHT_PIN & (1 << DHT_BIT))) { if (!tmo--) return 0; }
    tmo = 10000;
    while (!(DHT_PIN & (1 << DHT_BIT))) { if (!tmo--) return 0; }
    tmo = 10000;
    while ( (DHT_PIN & (1 << DHT_BIT))) { if (!tmo--) return 0; }

    // read 40 bits
    for (uint8_t i = 0; i < 5; i++) {
        for (uint8_t j = 0; j < 8; j++) {
            tmo = 10000;
            while (!(DHT_PIN & (1 << DHT_BIT))) { if (!tmo--) return 0; }
            _delay_us(40);
            if (DHT_PIN & (1 << DHT_BIT)) {
                data[i] |= (1 << (7 - j));
                tmo = 10000;
                while ((DHT_PIN & (1 << DHT_BIT))) { if (!tmo--) return 0; }
            }
        }
    }

    // checksum check
    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF))
        return 0;

    *humid = (float)((data[0] << 8) | data[1]) / 10.0f;
    int16_t raw = (int16_t)(((data[2] & 0x7F) << 8) | data[3]);
    if (data[2] & 0x80) raw = -raw;
    *temp = (float)raw / 10.0f;

    return 1;
}


// Servo on Timer1 OC1B (pin 12 = PB6) 
// fast PWM, TOP=ICR1=39999, prescaler 8 => 50Hz
// each tick is 0.5us
void servo_init(void) {
    DDRB   |= (1 << PB6);
    TCCR1A  = (1 << COM1B1) | (1 << WGM11);
    TCCR1B  = (1 << WGM13) | (1 << WGM12) | (1 << CS11);
    ICR1    = 39999;
    OCR1B   = 2999; // ~1.5ms neutral
}

void servo_angle(uint8_t angle) {
    uint16_t us = SERVO_MIN_US +
                  (uint16_t)((uint32_t)angle *
                  (SERVO_MAX_US - SERVO_MIN_US) / 180);
    OCR1B = (uint16_t)(us * 2) - 1;
}


// Buzzer 
void buzzer_init(void) {
    BUZ_DDR  |=  (1 << BUZ_BIT);
    BUZ_PORT &= ~(1 << BUZ_BIT);
}

ISR(TIMER2_COMPA_vect) {
    BUZ_PORT ^= (1 << BUZ_BIT);
}

void buzzer_tone_start(void) {
    TCCR2A = (1 << WGM21);
    TCCR2B = (1 << CS22); // prescaler 64
    OCR2A  = 124;          // 16M/64/125 = 2kHz toggle = 1kHz tone
    TIMSK2 = (1 << OCIE2A);
}

void buzzer_tone_stop(void) {
    TIMSK2 = 0;
    TCCR2B = 0;
    BUZ_PORT &= ~(1 << BUZ_BIT);
}

void buzzer_beep(uint16_t ms) {
    buzzer_tone_start();
    wait_ms(ms);
    buzzer_tone_stop();
}

void buzzer_beeps(uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        buzzer_beep(100);
        wait_ms(100);
    }
}


// RGB LED 
void rgb_init(void) {
    RGB_DDR_H |= (1 << RGB_R_BIT) | (1 << RGB_G_BIT);
    RGB_DDR_E |= (1 << RGB_B_BIT);
    RGB_PORT_H &= ~((1 << RGB_R_BIT) | (1 << RGB_G_BIT));
    RGB_PORT_E &= ~(1 << RGB_B_BIT);
}

void rgb_off(void) {
    RGB_PORT_H &= ~((1 << RGB_R_BIT) | (1 << RGB_G_BIT));
    RGB_PORT_E &= ~(1 << RGB_B_BIT);
}

void rgb_red(void)   { rgb_off(); RGB_PORT_H |= (1 << RGB_R_BIT); }
void rgb_green(void) { rgb_off(); RGB_PORT_H |= (1 << RGB_G_BIT); }
void rgb_blue(void)  { rgb_off(); RGB_PORT_E |= (1 << RGB_B_BIT); }

void rgb_flash_green(uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        rgb_green();
        wait_ms(200);
        rgb_off();
        wait_ms(200);
    }
}


// Button (pin 3 = PE5), active LOW with pullup 
void button_init(void) {
    BTN_DDR  &= ~(1 << BTN_BIT);
    BTN_PORT |=  (1 << BTN_BIT);
}

uint8_t button_pressed(void) {
    if (!(BTN_PIN & (1 << BTN_BIT))) {
        wait_ms(50); // debounce
        if (!(BTN_PIN & (1 << BTN_BIT))) return 1;
    }
    return 0;
}


// ADC for potentiometer on A0 
void adc_init(void) {
    ADMUX  = (1 << REFS0); // AVcc reference
    ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);
}

uint16_t adc_read(uint8_t ch) {
    ADMUX   = (ADMUX & 0xF8) | (ch & 0x07);
    ADCSRA |= (1 << ADSC);
    while (ADCSRA & (1 << ADSC));
    return ADC;
}


// next scheduled feeding time 
static void get_next_feed(uint8_t cur_h, uint8_t cur_m,
                          uint8_t *nh, uint8_t *nm) {
    uint16_t feeds[3] = {
        FEED1_H * 60 + FEED1_M,
        FEED2_H * 60 + FEED2_M,
        FEED3_H * 60 + FEED3_M
    };
    uint16_t now = (uint16_t)cur_h * 60 + cur_m;

    for (uint8_t i = 0; i < 3; i++) {
        if (feeds[i] > now) {
            *nh = feeds[i] / 60;
            *nm = feeds[i] % 60;
            return;
        }
    }
    // none left today, show tomorrows first feed
    *nh = FEED1_H;
    *nm = FEED1_M;
}


// what to show on each lcd screen 
void lcd_show(uint8_t screen) {
    lcd_clear();

    if (screen == 0) {
        // current time and date
        lcd_goto(0, 0);
        lcd_2d(g_time.hour);   lcd_char(':');
        lcd_2d(g_time.minute); lcd_char(':');
        lcd_2d(g_time.sec);
        lcd_goto(1, 0);
        lcd_2d(g_time.day);   lcd_char('/');
        lcd_2d(g_time.month); lcd_char('/');
        lcd_2d(g_time.year);
    }
    else if (screen == 1) {
        // sensor readings
        lcd_goto(0, 0);
        if (g_dht_ok) {
            lcd_str("Temp: ");
            lcd_float(g_temp);
            lcd_char('C');
            lcd_goto(1, 0);
            lcd_str("Humid:");
            lcd_float(g_humid);
            lcd_char('%');
        } else {
            lcd_str("Sensor Error!");
            lcd_goto(1, 0);
            lcd_str("Check DHT-22");
        }
    }
    else if (screen == 2) {
        uint8_t nh = 0, nm = 0;
        get_next_feed(g_time.hour, g_time.minute, &nh, &nm);
        lcd_goto(0, 0);
        lcd_str("Next Feed:");
        lcd_goto(1, 0);
        lcd_2d(nh); lcd_char(':'); lcd_2d(nm);
        lcd_str(" Scheduled");
    }
    else {
        // status screen
        lcd_goto(0, 0);
        lcd_str("Status:");
        lcd_goto(1, 0);
        if (g_state == STANDBY) {
            lcd_str("STANDBY      ");
            rgb_off();
        } else if (g_state == FEEDING) {
            lcd_str("FEEDING!     ");
            rgb_green();
        } else {
            lcd_str("SENSOR ERROR!");
            rgb_red();
        }
    }
}


// run a feeding cycle 
void do_feed(const char *label) {
    g_state = FEEDING;

    lcd_clear();
    lcd_goto(0, 0); lcd_str("** FEEDING **");
    lcd_goto(1, 0); lcd_str(label);

    rgb_blue();
    buzzer_beeps(3);

    // swing servo open and close
    servo_angle(0);   wait_ms(400);
    servo_angle(60);  wait_ms(400);
    servo_angle(120); wait_ms(700);
    servo_angle(60);  wait_ms(400);
    servo_angle(0);   wait_ms(400);

    rgb_flash_green(4);
    buzzer_beep(400);
    wait_ms(1000);

    g_state     = STANDBY;
    g_screen_ms = get_ms();
}


int main(void) {

    timer0_init();
    sei();

    i2c_init();
    lcd_init();

    buzzer_init();
    rgb_init();
    button_init();
    adc_init();
    servo_init();
    servo_angle(0);

    DHT_DDR  &= ~(1 << DHT_BIT);
    DHT_PORT |=  (1 << DHT_BIT);

    
    rtc_set(8, 0, 0, 1, 1, 26);

    lcd_clear();
    lcd_goto(0, 0); lcd_str("  Fish Feeder");
    lcd_goto(1, 0); lcd_str(" Starting...");
    rgb_blue();
    wait_ms(2000);
    rgb_off();

    rtc_read(&g_time);
    g_dht_ok = dht22_read(&g_temp, &g_humid);

    g_screen    = 0;
    g_screen_ms = get_ms();
    lcd_show(0);

    uint32_t last_rtc_ms = 0;
    uint32_t last_dht_ms = 0;

    while (1) {

        uint32_t now = get_ms();

        // read rtc every second
        if ((now - last_rtc_ms) >= 1000) {
            rtc_read(&g_time);
            last_rtc_ms = now;
        }

        // dht22 only every 3s, sensor doesnt like being read too fast
        if ((now - last_dht_ms) >= 3000) {
            uint8_t ok = dht22_read(&g_temp, &g_humid);
            if (ok) {
                g_dht_ok = 1;
                if (g_state == ERROR) g_state = STANDBY;
            } else {
                g_dht_ok = 0;
                g_state  = ERROR;
                rgb_red();
            }
            last_dht_ms = now;
        }

        // check if its time to feed
        if (g_state == STANDBY) {
            if (g_time.hour == FEED1_H && g_time.minute == FEED1_M
                    && g_last_fed_min != g_time.minute) {
                g_last_fed_min = g_time.minute;
                do_feed("Morning 08:00");
            }
            else if (g_time.hour == FEED2_H && g_time.minute == FEED2_M
                    && g_last_fed_min != g_time.minute) {
                g_last_fed_min = g_time.minute;
                do_feed("Noon    12:00");
            }
            else if (g_time.hour == FEED3_H && g_time.minute == FEED3_M
                    && g_last_fed_min != g_time.minute) {
                g_last_fed_min = g_time.minute;
                do_feed("Evening 18:00");
            }
            else if (g_time.minute != FEED1_M &&
                     g_time.minute != FEED2_M &&
                     g_time.minute != FEED3_M) {
                g_last_fed_min = 255; // reset so next feed works
            }
        }

        // manual button feed
        if (button_pressed() && g_state == STANDBY) {
            do_feed("Manual Feed");
        }

        // cycle lcd screen
        if ((now - g_screen_ms) >= SCREEN_DURATION) {
            g_screen = (g_screen + 1) % SCREEN_COUNT;
            g_screen_ms = now;
        }

        if (g_state != FEEDING) {
            lcd_show(g_screen);
        }

        _delay_ms(200);
    }

    return 0;
}