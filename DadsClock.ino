#include <EEPROM.h>
#include <Time.h>
#include <Wire.h>

#include <DS1307RTC.h>
#include <LedControl.h>

// After power-on, we fade in.  These parameters control that.
#define FADE_ON_TIME 2000 // ms for the whole thing
#define FADE_ON_MAX 15    // max value (min is always 0)
// We have a delay in the main loop to lower power consumption by not polling
// the RTC constantly.
// We don't use an interrupt, since the RTC keeps sending interrupts
// while Vcc is unpowered, doubling the drain on the lithium backup battery.
#define LOOP_TIME 100 // milliseconds to delay per loop

// These are the Arduino pin numbers.
#define LCL_LOAD 10 //MAX7219 pin 12
#define LCL_CLK 11  //MAX7219 pin 13
#define LCL_DIN 12  //MAX7219 pin 1
#define RTC_SQW 2 // RTC square wave output; currently unused
// There's a Daylight Saving Time switch, active low.
// It has an internal pull-up, so ground the pin for DST (in the summer).
// We always store the time as standard time.
#define DST_PIN 7 // Daylight Saving Time switch

#ifndef DS1307_CTRL_ID
# define DS1307_CTRL_ID 0x68
#endif

LedControl lcl = LedControl(LCL_DIN, LCL_CLK, LCL_LOAD, 1);

// There's a debug flag that will:
// * Toggle the status LED every loop, to get a sense of how fast it's
//   running
// * Print (to the serial port) the time
// * Print the display intensity during fade-in
// * Print the full contents of the RTC during startup
// * Maybe other stuff
// We control this using a byte flag in EEPROM (255 from the factory).
// Toggle it by sending "D\r" on the serial port to turn it on, or "d\r"
// to turn it off.
#define DEBUG_ADDR 0 // address in EEPROM of the debug flag
bool debug;

#define LOOP_ADDR 1 // address in EEPROM of the loop choice flag
void normalInnerLoop();
void burnTestInnerLoop();
void eightsInnerLoop();
#define MAX_LOOPNUM 2
void (*innerLoops[MAX_LOOPNUM+1])(void) =
	{normalInnerLoop, burnTestInnerLoop, eightsInnerLoop};
void (*innerLoop)() = normalInnerLoop;

void
setup()
{
	debug = EEPROM.read(DEBUG_ADDR);
	pinMode(LED_BUILTIN, OUTPUT);
	if (debug) {
		digitalWrite(LED_BUILTIN, HIGH);
	}
	Serial.begin(9600);
	
	unsigned int innerLoopNum = EEPROM.read(LOOP_ADDR);
	if (innerLoopNum > MAX_LOOPNUM) {
		innerLoopNum = 0;
	}
	innerLoop = innerLoops[innerLoopNum];

	pinMode(DST_PIN, INPUT_PULLUP);

	// Set to displaying nothing, lowest intensity, then turn on the display.
	lcl.clearDisplay(0);
	lcl.setIntensity(0, 0);
	lcl.shutdown(0, false);

	if (debug) {
		Wire.beginTransmission(DS1307_CTRL_ID);
		Wire.write((uint8_t)0x00); // reset register pointer
		Wire.endTransmission();
		unsigned char bytes[0x40];
		int i;
		Wire.requestFrom(DS1307_CTRL_ID, 0x40);
		for (i = 0; i < 0x40; i++) {
			bytes[i] = Wire.read();
		}
		Serial.println(F("RTC:"));
		for (i = 0; i < 0x40; i++) {
			Serial.print(bytes[i], 16);
			Serial.write(' ');
		}
		Serial.println("");

		Serial.println(F("go"));
		digitalWrite(LED_BUILTIN, LOW);
	}
}

bool
inDST()
{
	return digitalRead(DST_PIN) == LOW;
}

// See BSD's date(1) for the format this uses.  It's pretty much
//   yyyymmddHHMM.SS
// except that the most signifiant parts can be omitted (defaults to no change),
// and the seconds can be omitted (defaults to 0).  The minimal form
// would be "MM" to just update the minutes and set seconds to 0.
void
read_and_set_time(const String &str)
{
	tmElements_t tm;

	RTC.read(tm);
	int start = 0;
	int dotpos = str.indexOf('.');
	int datelen = dotpos != -1 ? dotpos : str.length();

	if (dotpos == -1) {
		tm.Second = 0;
	} else {
		tm.Second = str.substring(dotpos + 1).toInt();
	}

#define GETDD(dd) \
			do { \
				dd = str.substring(start, start+2).toInt(); \
				start += 2; \
				datelen -= 2; \
			} while (0)
	int calYear = tmYearToCalendar(tm.Year);
	if (datelen >= 12) {
		int cc;
		GETDD(cc);
		calYear = cc * 100;
	}
	if (datelen >= 10) {
		int yy;
		GETDD(yy);
		calYear = (calYear / 100) * 100 + yy;
	}
	tm.Year = CalendarYrToTm(calYear);
	if (datelen >= 8) GETDD(tm.Month);
	if (datelen >= 6) GETDD(tm.Day);
	if (datelen >= 4) {
		GETDD(tm.Hour);
		// If we're in DST, then the input value was an hour ahead of
		// standard time.  Compensate.
		if (inDST()) {
			if (tm.Hour == 0) {
				tm.Hour = 23;
				tm.Day--; // Not guaranteed to work; don't care right now
			} else {
				tm.Hour--;
			}
		}
	}
	if (datelen >= 2) GETDD(tm.Minute);
#undef GETDD

	if (datelen != 0) {
		Serial.println(F("? ccyymmddHHMM.SS"));
		Serial.println(str);
		Serial.println(datelen);
		Serial.println(dotpos);
		Serial.println(str.length());
		return;
	}
	RTC.write(tm);
}

void
xmitTime(char hrTens, char hrOnes, char minTens, char minOnes,
         char secTens, char secOnes, bool colon, bool pm)
{
	if (!debug)
		return;
	Serial.write(hrTens);
	Serial.write(hrOnes);
	Serial.write(colon ? ':' : ' ');
	Serial.write(minTens);
	Serial.write(minOnes);
	Serial.println(pm ? F(" PM") : F(" AM"));
}

#define SEG_A 0x02
#define SEG_B 0x40
#define SEG_C 0x20
#define SEG_D 0x10
#define SEG_E 0x08
#define SEG_F 0x04
#define SEG_G 0x01
#define SEG_DP 0x80
static const unsigned char display_seg_lut[10] =
{
	/*0*/ SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
	/*1*/ SEG_B | SEG_C,
	/*2*/ SEG_A | SEG_B | SEG_D | SEG_E | SEG_G,
	/*3*/ SEG_A | SEG_B | SEG_C | SEG_D | SEG_G,
	/*4*/ SEG_B | SEG_C | SEG_F | SEG_G,
	/*5*/ SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,
	/*6*/ SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
	/*7*/ SEG_A | SEG_B | SEG_C,
	/*8*/ SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
	/*9*/ SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G
};

// I hooked up the segments wrong, apparently, so I do the digit conversion
// myself to change their order.
void
lutSetChar(int controller, int position, unsigned char value, bool dp)
{
	unsigned char display;
	switch (value) {
	case ' ':
		value = 0; break;
	case '0'...'9':
		value = display_seg_lut[value - '0']; break;
	case 0 ... 9:
		value = display_seg_lut[value]; break;
	}
	if (dp)
		value |= SEG_DP;
	lcl.setRow(controller, position, value);
}

void
displayTime(char hrTens, char hrOnes, char minTens, char minOnes,
		    char secTens, char secOnes, bool colon, bool pm)
{
#if TESTPAT_EIGHTS
	int i;
	for (i = 0; i < 4; i++) {
		lutSetChar(0, i, colon ? '8' : ' ', colon);
	}
#elif TESTPAT_DIGITS
	for (i = 0; i < 4; i++) {
		lutSetChar(0, i, secOnes, colon);
	}
#else
	lutSetChar(0, 0, hrTens, 0);
	lutSetChar(0, 1, hrOnes, colon);
	lutSetChar(0, 2, minTens, colon);
	lutSetChar(0, 3, minOnes, pm);
#endif
}

void
showTime(char hrTens, char hrOnes, char minTens, char minOnes,
		 char secTens, char secOnes, bool colon, bool pm)
{
	xmitTime(hrTens, hrOnes, minTens, minOnes, secTens, secOnes, colon, pm);
	displayTime(hrTens, hrOnes, minTens, minOnes, secTens, secOnes, colon, pm);
}

void
handleSerialInput()
{
	while (1) {
		switch (Serial.read())
		{
			case -1:
				return;
			case 'T':
				read_and_set_time(Serial.readStringUntil('\r'));
				break;
			case 'D':
				debug = true;
				EEPROM.write(DEBUG_ADDR, 1);
				Serial.readStringUntil('\r');
				break;
			case 'd':
				debug = false;
				EEPROM.write(DEBUG_ADDR, 0);
				digitalWrite(LED_BUILTIN, LOW);
				Serial.readStringUntil('\r');
				break;
			case 'l':
				{
					unsigned char loopNum = Serial.read() - '0';
					if (loopNum > MAX_LOOPNUM) {
						Serial.println('?');
						break;
					}
					innerLoop = innerLoops[loopNum];
					EEPROM.write(LOOP_ADDR, loopNum);
					Serial.readStringUntil('\r');
					break;
				}
			case '\r':
			case '\n':
				Serial.println('?');
				break;
			case '?':
				Serial.println(F("Debug: D (on) or d (off)\r\n"
								 "Set: T[[[[[cc]yy]mm]dd]HH]MM[.ss]"));
				break;
			default:
				break;
		}
	}
}

void
normalInnerLoop()
{
	tmElements_t tm;
	RTC.read(tm);
	static int lastColon = -1;
	int colon = tm.Second % 2 == 0 ? 1 : 0;
	if (colon != lastColon)
	{
		lastColon = colon;

		int dstHour = tm.Hour + (inDST() ? 1 : 0);
		int displayHour = dstHour % 12;
		if (displayHour == 0)
		{
			displayHour = 12;
		}
		showTime(displayHour < 10 ? ' ' : '1', displayHour % 10 + '0',
		         tm.Minute / 10 + '0', tm.Minute % 10 + '0',
		         tm.Second / 10 + '0', tm.Second % 10 + '0',
		         colon, dstHour >= 12);
	}
	
	// Loop more quickly during the fade up.
	static int last_intensity = -1;
	if (last_intensity != FADE_ON_MAX)
		delay(FADE_ON_TIME / (FADE_ON_MAX * 4));
	else
		delay(LOOP_TIME);
	
	if (last_intensity != FADE_ON_MAX)
	{
		int intensity = millis() * FADE_ON_MAX / FADE_ON_TIME;
		if (intensity > FADE_ON_MAX)
			intensity = FADE_ON_MAX;
		if (intensity != last_intensity)
		{
			last_intensity = intensity;
			lcl.setIntensity(0, intensity);
			if (debug) {
				Serial.print(F("* "));
				Serial.println(intensity);
			}
		}
	}

	return;

#if 0
	// An earlier draft of this code landed here if the read failed.  It
	// would print 0s if the chip was missing, and flash 12s if the clock
	// wasn't running (CH, register 0 bit 7; off from the factory).
	// But, detecting that requires a newer version of the DS1307RTC library
	// than codebender currently has installed.
	if (!RTC.chipPresent()) {
		Serial.println(F("00:00"));
		delay(10000);
		return;
	}
	if (colon)
		Serial.println(F("12:00"));
	else
		Serial.println("");
	colon = !colon;
	delay(1000);
	return;
#endif
}

void
burnTestInnerLoop()
{
	lcl.setIntensity(0, 15);
	for (int i = 0; i < 8; i++) {
		lcl.setRow(0, 0, 0x01 << i);
		lcl.setRow(0, 1, 0x01 << i);
		lcl.setRow(0, 2, 0x01 << i);
		lcl.setRow(0, 3, 0x01 << i);
		delay(250);
	}
}

void
eightsInnerLoop()
{
	lcl.setIntensity(0, 15);
	lcl.setRow(0, 0, 0xFF);
	lcl.setRow(0, 1, 0xFF);
	lcl.setRow(0, 2, 0xFF);
	lcl.setRow(0, 3, 0xFF);
	delay(250);
}

void
loop()
{
	static bool status;
	if (debug) {
		digitalWrite(LED_BUILTIN, status ? HIGH : LOW);
		status = !status;
	}
	handleSerialInput();
	innerLoop();
}
