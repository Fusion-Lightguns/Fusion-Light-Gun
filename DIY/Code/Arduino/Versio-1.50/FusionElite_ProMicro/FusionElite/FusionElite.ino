/*!
 * @file FusionElite.ino
 * @brief Fusion Lightgun code. 
 *
 *
 * @copyright Gonezo Fusion Lightguns
 * @copyright GNU Lesser General Public License
 *
 * @author [Sam Ballantyne](samuelballantyne@hotmail.com)
 * @author Mike Lynch
 * @author Gonezo
 * @version V1.5
 * @date 2023
  */

 /* Default button assignments
 *
 *  Calibrate will enter Pause mode.
 * 
 *  In Pause mode:
 *  A, B, Start, Select : select a profile
 *  Start + Select: Normal gun mode (averaging disabled)
 *  Start + A: Normal gun with averaging, toggles between the 2 averaging modes (use serial monitor to see the setting)
 *  Start + B: Processing mode for use with the Processing sketch
 *  B + A: Decrease IR camera sensitivity (use serial monitor to see the setting)
 *  B + Select: Increase IR camera sensitivity (use serial monitor to see the setting)
 *  Calibrate: Exit pause mode
 *  Trigger: Begin calibration
 *  A + Select: save settings to non-volatile memory
 *
 *  Note that the buttons in pause mode (and to enter pause mode) activate when the last button of
 *  the comination releases.
 *  This is used to detect and differentiate button combinations vs a single button press.
*/


#include <Arduino.h>
#include <EliteBoard.h>

// include TinyUSB or HID depending on USB stack option
#if defined(USE_TINYUSB)
#include <Adafruit_TinyUSB.h>
#elif defined(CFG_TUSB_MCU)
#error Incompatible USB stack. Use Arduino or Adafruit TinyUSB.
#else
// Arduino USB stack
#include <HID.h>
#endif

#include <Keyboard.h>
#include <Wire.h>
#ifdef DOTSTAR_ENABLE
#include <Adafruit_DotStar.h>
#endif // DOTSTAR_ENABLE
#ifdef NEOPIXEL_PIN
#include <Adafruit_NeoPixel.h>
#endif
#ifdef ELITE_FLASH_ENABLE
#include <Adafruit_SPIFlashBase.h>
#endif // ELITE_FLASH_ENABLE
#include <AbsMouse5.h>
#include <DFRobotIRPositionEx.h>
#include <LightgunButtons.h>
#include <ElitePositionEnhanced.h>
#include <EliteConst.h>
#include "EliteColours.h"
#include "ElitePreferences.h"

#ifdef ARDUINO_ARCH_RP2040
#include <hardware/pwm.h>
#include <hardware/irq.h>

// declare PWM ISR
void rp2040pwmIrq(void);
#endif

// enable extra serial debug during run mode
//#define PRINT_VERBOSE 1
//#define DEBUG_SERIAL 1
//#define DEBUG_SERIAL 2

// numbered index of buttons, must match ButtonDesc[] order
enum ButtonIndex_e {
    BtnIdx_Trigger = 0,
    BtnIdx_A,
    BtnIdx_B,
    BtnIdx_Start,
    BtnIdx_Select,
    BtnIdx_Calibrate,
	BtnIdx_Up,
    BtnIdx_Down,
    BtnIdx_Left,
    BtnIdx_Right,
    BtnIdx_Reload
};

// bit mask for each button, must match ButtonDesc[] order to match the proper button events
enum ButtonMask_e {
    BtnMask_Trigger = 1 << BtnIdx_Trigger,
    BtnMask_A = 1 << BtnIdx_A,
    BtnMask_B = 1 << BtnIdx_B,
    BtnMask_Start = 1 << BtnIdx_Start,
    BtnMask_Select = 1 << BtnIdx_Select,
    BtnMask_Calibrate = 1 << BtnIdx_Calibrate,
	BtnMask_Up = 1 << BtnIdx_Up,
    BtnMask_Down = 1 << BtnIdx_Down,
    BtnMask_Left = 1 << BtnIdx_Left,
    BtnMask_Right = 1 << BtnIdx_Right,
    BtnMask_Reload = 1 << BtnIdx_Reload
};

// Button descriptor
// The order of the buttons is the order of the button bitmask
const LightgunButtons::Desc_t LightgunButtons::ButtonDesc[] = {
    {10, LightgunButtons::ReportType_Mouse, MOUSE_LEFT, 20, BTN_AG_MASK, "Trigger"},
    {9, LightgunButtons::ReportType_Keyboard, KEY_RETURN, 25, BTN_AG_MASK2, "A"},
    {8, LightgunButtons::ReportType_Keyboard, KEY_ESC, 25, BTN_AG_MASK2, "B"},
    {7, LightgunButtons::ReportType_Keyboard, KEY_1, 20, BTN_AG_MASK2, "Start"},
    {6, LightgunButtons::ReportType_Keyboard, KEY_5, 25, BTN_AG_MASK2, "Select"},
    {14, LightgunButtons::ReportType_Keyboard, KEY_F4, 20, BTN_AG_MASK2, "Calibrate"},
    {A0, LightgunButtons::ReportType_Keyboard, KEY_UP_ARROW, 25, BTN_AG_MASK2, "Up"},
    {A1, LightgunButtons::ReportType_Keyboard, KEY_DOWN_ARROW, 25, BTN_AG_MASK2, "Down"},
    {A2, LightgunButtons::ReportType_Keyboard, KEY_LEFT_ARROW, 25, BTN_AG_MASK2, "Left"},
    {A3, LightgunButtons::ReportType_Keyboard, KEY_RIGHT_ARROW, 25, BTN_AG_MASK2, "Right"},
    {16, LightgunButtons::ReportType_Mouse, MOUSE_RIGHT, 20, BTN_AG_MASK2, "Reload"}
};

// button count constant


constexpr unsigned int ButtonCount = sizeof(LightgunButtons::ButtonDesc) / sizeof(LightgunButtons::ButtonDesc[0]);

// button runtime data arrays
LightgunButtonsStatic<ButtonCount> lgbData;

// button object instance
LightgunButtons buttons(lgbData, ButtonCount);


// button combo to enter pause mode
constexpr uint32_t EnterPauseModeBtnMask = BtnMask_Calibrate;

// press any button to enter pause mode from Processing mode (this is not a button combo)
constexpr uint32_t EnterPauseModeProcessingBtnMask = BtnMask_A | BtnMask_B | BtnMask_Calibrate;

// button combo to exit pause mode back to run mode
constexpr uint32_t ExitPauseModeBtnMask = BtnMask_Calibrate;

// press any button to cancel the calibration (this is not a button combo)
constexpr uint32_t CancelCalBtnMask = BtnMask_Calibrate;

// button combo to skip the center calibration step
constexpr uint32_t SkipCalCenterBtnMask = BtnMask_A;

// button combo to save preferences to non-volatile memory
constexpr uint32_t SaveBtnMask = BtnMask_A | BtnMask_Select;

// button combo to increase IR sensitivity
constexpr uint32_t IRSensitivityUpBtnMask = BtnMask_B | BtnMask_Select;

// button combo to decrease IR sensitivity
constexpr uint32_t IRSensitivityDownBtnMask = BtnMask_B | BtnMask_A;

// button combinations to select a run mode
constexpr uint32_t RunModeNormalBtnMask = BtnMask_Start | BtnMask_Select;
constexpr uint32_t RunModeAverageBtnMask = BtnMask_Start | BtnMask_A;
constexpr uint32_t RunModeProcessingBtnMask = BtnMask_Start | BtnMask_B;

// colour when no IR points are seen
constexpr uint32_t IRSeen0Color = WikiColor::Red;

// colour when calibrating
constexpr uint32_t CalModeColor = WikiColor::Cornflower_blue;

// number of profiles
constexpr unsigned int ProfileCount = 8;

// run modes
// note that this is a 5 bit value when stored in the profiles
enum RunMode_e {
    RunMode_Normal = 0,         ///< Normal gun mode, no averaging
    RunMode_Average = 1,        ///< 2 frame moving average
    RunMode_Average2 = 2,       ///< weighted average with 3 frames
    RunMode_ProfileMax = 2,     ///< maximum mode allowed for profiles
    RunMode_Processing = 3,     ///< Processing test mode
    RunMode_Count
};

// profiles
// defaults can be populated here, or not worry about these values and just save to flash/EEPROM
// if you have original Elite calibration values, multiply by 4 for the center position and
// scale is multiplied by 1000 and stored as an unsigned integer, see ElitePreferences::Calibration_t
ElitePreferences::ProfileData_t profileData[ProfileCount] = {
    {1619, 950, MouseMaxX / 2, MouseMaxY / 2, DFRobotIRPositionEx::Sensitivity_Default, RunMode_Normal, 0, 0},
    {1233, 950, MouseMaxX / 2, MouseMaxY / 2, DFRobotIRPositionEx::Sensitivity_Default, RunMode_Normal, 0, 0},
    {1538, 855, MouseMaxX / 2, MouseMaxY / 2, DFRobotIRPositionEx::Sensitivity_Default, RunMode_Normal, 0, 0},
    {1147, 855, MouseMaxX / 2, MouseMaxY / 2, DFRobotIRPositionEx::Sensitivity_Default, RunMode_Normal, 0, 0},
    {0, 0, 0, 0, DFRobotIRPositionEx::Sensitivity_Default, RunMode_Normal, 0, 0},
    {0, 0, 0, 0, DFRobotIRPositionEx::Sensitivity_Default, RunMode_Normal, 0, 0},
    {0, 0, 0, 0, DFRobotIRPositionEx::Sensitivity_Default, RunMode_Normal, 0, 0},
    {0, 0, 0, 0, DFRobotIRPositionEx::Sensitivity_Default, RunMode_Normal, 0, 0}
};
/*ElitePreferences::ProfileData_t profileData[profileCount] = {
    {1619, 950, 1899, 1531, DFRobotIRPositionEx::Sensitivity_Max, RunMode_Average},
    {1233, 943, 1864, 1538, DFRobotIRPositionEx::Sensitivity_Max, RunMode_Average},
    {1538, 855, 1878, 1515, DFRobotIRPositionEx::Sensitivity_High, RunMode_Average},
    {1147, 855, 1889, 1507, DFRobotIRPositionEx::Sensitivity_High, RunMode_Average},
    {0, 0, 0, 0, DFRobotIRPositionEx::Sensitivity_Default, RunMode_Normal},
    {0, 0, 0, 0, DFRobotIRPositionEx::Sensitivity_Default, RunMode_Normal},
    {0, 0, 0, 0, DFRobotIRPositionEx::Sensitivity_Default, RunMode_Normal},
    {0, 0, 0, 0, DFRobotIRPositionEx::Sensitivity_Default, RunMode_Normal}
};*/

// profile descriptor
typedef struct ProfileDesc_s {
    // button(s) to select the profile
    uint32_t buttonMask;
    
    // LED colour
    uint32_t color;

    // button label
    const char* buttonLabel;
    
    // optional profile label
    const char* profileLabel;
} ProfileDesc_t;

// profile descriptor
static const ProfileDesc_t profileDesc[ProfileCount] = {
    {BtnMask_A, WikiColor::Cerulean_blue, "A", "TV"},
    {BtnMask_B, WikiColor::Cornflower_blue, "B", "TV 4:3"},
    {BtnMask_Start, WikiColor::Green, "Start", "Monitor"},
    {BtnMask_Select, WikiColor::Green_Lizard, "Select", "Monitor 4:3"},
};

// overall calibration defaults, no need to change if data saved to NV memory or populate the profile table
// see profileData[] array below for specific profile defaults
int xCenter = MouseMaxX / 2;
int yCenter = MouseMaxY / 2;
float xScale = 1.64;
float yScale = 0.95;

// step size for adjusting the scale
constexpr float ScaleStep = 0.001;

int finalX = 0;         // Values after tilt correction
int finalY = 0;

int moveXAxis = 0;      // Unconstrained mouse postion
int moveYAxis = 0;               
int moveXAxisArr[3] = {0, 0, 0};
int moveYAxisArr[3] = {0, 0, 0};
int moveIndex = 0;

int conMoveXAxis = 0;   // Constrained mouse postion
int conMoveYAxis = 0;

unsigned int lastSeen = 0;

#ifdef EXTRA_POS_GLITCH_FILTER
int badFinalTick = 0;
int badMoveTick = 0;
int badFinalCount = 0;
int badMoveCount = 0;

// number of consecutive bad move values to filter
constexpr unsigned int BadMoveCountThreshold = 3;

// Used to filter out large jumps/glitches
constexpr int BadMoveThreshold = 49 * CamToMouseMult;
#endif // EXTRA_POS_GLITCH_FILTER

// profile in use
unsigned int selectedProfile = 0;

// IR positioning camera
#ifdef ARDUINO_ADAFRUIT_ITSYBITSY_RP2040
DFRobotIRPositionEx dfrIRPos(Wire1);
#else
//DFRobotIRPosition myDFRobotIRPosition;
DFRobotIRPositionEx dfrIRPos(Wire);
#endif

// Elite positioning
ElitePositionEnhanced myElite;

// operating modes
enum GunMode_e {
    GunMode_Init = -1,
    GunMode_Run = 0,
    GunMode_CalHoriz = 1,
    GunMode_CalVert = 2,
    GunMode_CalCenter = 3,
    GunMode_Pause = 4
};
GunMode_e gunMode = GunMode_Init;   // initial mode

// run mode
RunMode_e runMode = RunMode_Normal;

// IR camera sensitivity
DFRobotIRPositionEx::Sensitivity_e irSensitivity = DFRobotIRPositionEx::Sensitivity_Default;

static const char* RunModeLabels[RunMode_Count] = {
    "Normal",
    "Averaging",
    "Averaging2",
    "Processing"
};

// preferences saved in non-volatile memory, populated with defaults 
ElitePreferences::Preferences_t ElitePreferences::preferences = {
    profileData, ProfileCount, // profiles
    0, // default profile
};

enum StateFlag_e {
    // print selected profile once per pause state when the COM port is open
    StateFlag_PrintSelectedProfile = (1 << 0),
    
    // report preferences once per pause state when the COM port is open
    StateFlag_PrintPreferences = (1 << 1),
    
    // enable save (allow save once per pause state)
    StateFlag_SavePreferencesEn = (1 << 2),
    
    // print preferences storage
    StateFlag_PrintPreferencesStorage = (1 << 3)
};

// when serial connection resets, these flags are set
constexpr uint32_t StateFlagsDtrReset = StateFlag_PrintSelectedProfile | StateFlag_PrintPreferences | StateFlag_PrintPreferencesStorage;

// state flags, see StateFlag_e
uint32_t stateFlags = StateFlagsDtrReset;


#ifdef DOTSTAR_ENABLE
// note if the colours don't match then change the colour format from BGR
// apparently different lots of DotStars may have different colour ordering ¯\_(ツ)_/¯
Adafruit_DotStar dotstar(1, DOTSTAR_DATAPIN, DOTSTAR_CLOCKPIN, DOTSTAR_BGR);
#endif // DOTSTAR_ENABLE

#ifdef NEOPIXEL_PIN
Adafruit_NeoPixel neopixel(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
#endif // NEOPIXEL_PIN

// flash transport instance
#if defined(EXTERNAL_FLASH_USE_QSPI)
    Adafruit_FlashTransport_QSPI flashTransport;
#elif defined(EXTERNAL_FLASH_USE_SPI)
    Adafruit_FlashTransport_SPI flashTransport(EXTERNAL_FLASH_USE_CS, EXTERNAL_FLASH_USE_SPI);
#endif

#ifdef ELITE_FLASH_ENABLE
// Adafruit_SPIFlashBase non-volatile storage
// flash instance
Adafruit_SPIFlashBase flash(&flashTransport);

static const char* NVRAMlabel = "Flash";

// flag to indicate if non-volatile storage is available
// this will enable in setup()
bool nvAvailable = false;
#endif // ELITE_FLASH_ENABLE

#ifdef ELITE_EEPROM_ENABLE
// EEPROM non-volatile storage
static const char* NVRAMlabel = "EEPROM";

// flag to indicate if non-volatile storage is available
// unconditional for EEPROM
bool nvAvailable = true;
#endif

// non-volatile preferences error code
int nvPrefsError = ElitePreferences::Error_NoStorage;

// preferences instance
ElitePreferences elitePreferences;

// number of times the IR camera will update per second
constexpr unsigned int IRCamUpdateRate = 209;

#ifdef ELITE_NO_HW_TIMER
// use the millis() or micros() counter instead
unsigned long irPosUpdateTime = 0;
// will set this to 1 when the IR position can update
unsigned int irPosUpdateTick = 0;

#define ELITE_NO_HW_TIMER_UPDATE() NoHardwareTimerCamTickMillis()
//define ELITE_NO_HW_TIMER_UPDATE() NoHardwareTimerCamTickMicros()

#else
#define ELITE_NO_HW_TIMER_UPDATE()
// timer will set this to 1 when the IR position can update
volatile unsigned int irPosUpdateTick = 0;
#endif // ELITE_NO_HW_TIMER

#ifdef DEBUG_SERIAL
static unsigned long serialDbMs = 0;
static unsigned long frameCount = 0;
static unsigned long irPosCount = 0;
#endif

// used for periodic serial prints
unsigned long lastPrintMillis = 0;

#ifdef USE_TINYUSB

// USB HID Report ID
enum HID_RID_e{
  HID_RID_KEYBOARD = 1,
  HID_RID_MOUSE
};

// HID report descriptor using TinyUSB's template
uint8_t const desc_hid_report[] = {
  TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_RID_KEYBOARD)),
  TUD_HID_REPORT_DESC_ABSMOUSE5(HID_REPORT_ID(HID_RID_MOUSE))
};

Adafruit_USBD_HID usbHid;

int __USBGetKeyboardReportID()
{
    return HID_RID_KEYBOARD;
}

// AbsMouse5 instance
AbsMouse5_ AbsMouse5(HID_RID_MOUSE);

#else
// AbsMouse5 instance
AbsMouse5_ AbsMouse5(1);
#endif

void setup() {
  // Initialize Button Pins
  pinMode(9, INPUT_PULLUP);
  pinMode(10, INPUT_PULLUP);
  pinMode(11, INPUT_PULLUP);
  pinMode(12, INPUT_PULLUP);

    // init DotStar and/or NeoPixel to red during setup()
#ifdef DOTSTAR_ENABLE
    dotstar.begin();
    dotstar.setPixelColor(0, 150, 0, 0);
    dotstar.show();
#endif // DOTSTAR_ENABLE
#ifdef NEOPIXEL_ENABLEPIN
    pinMode(NEOPIXEL_ENABLEPIN, OUTPUT);
    digitalWrite(NEOPIXEL_ENABLEPIN, HIGH);
#endif // NEOPIXEL_ENABLEPIN
#ifdef NEOPIXEL_PIN
    neopixel.begin();
    neopixel.setPixelColor(0, 255, 0, 0);
    neopixel.show();
#endif // NEOPIXEL_PIN

#ifdef ARDUINO_ADAFRUIT_ITSYBITSY_RP2040
    // ensure Wire1 SDA and SCL are correct
    Wire1.setSDA(2);
    Wire1.setSCL(3);
#endif

    // initialize buttons
    buttons.Begin();

#ifdef ELITE_FLASH_ENABLE
    // init flash and load saved preferences
    nvAvailable = flash.begin();
#endif // ELITE_FLASH_ENABLE
    
    if(nvAvailable) {
        LoadPreferences();
    }

    // use values from preferences
    ApplyInitialPrefs();

    // Start IR Camera with basic data format
    dfrIRPos.begin(DFROBOT_IR_IIC_CLOCK, DFRobotIRPositionEx::DataFormat_Basic, irSensitivity);
    
#ifdef USE_TINYUSB
    usbHid.setPollInterval(2);
    usbHid.setReportDescriptor(desc_hid_report, sizeof(desc_hid_report));
    //usb_hid.setStringDescriptor("TinyUSB HID Composite");

    usbHid.begin();
#endif

    Serial.begin(115200);
    
    AbsMouse5.init(MouseMaxX, MouseMaxY, true);
   
    // sanity to ensure the cal prefs is populated with at least 1 entry
    // in case the table is zero'd out
    if(profileData[selectedProfile].xCenter == 0) {
        profileData[selectedProfile].xCenter = xCenter;
    }
    if(profileData[selectedProfile].yCenter == 0) {
        profileData[selectedProfile].yCenter = yCenter;
    }
    if(profileData[selectedProfile].xScale == 0) {
        profileData[selectedProfile].xScale = CalScaleFloatToPref(xScale);
    }
    if(profileData[selectedProfile].yScale == 0) {
        profileData[selectedProfile].yScale = CalScaleFloatToPref(yScale);
    }
    
    // fetch the calibration data, other values already handled in ApplyInitialPrefs() 
    SelectCalPrefs(selectedProfile);

#ifdef USE_TINYUSB
    // wait until device mounted
    while(!USBDevice.mounted()) { yield(); }
#else
    // was getting weird hangups... maybe nothing, or maybe related to dragons, so wait a bit
    delay(100);
#endif

    // IR camera maxes out motion detection at ~300Hz, and millis() isn't good enough
    startIrCamTimer(IRCamUpdateRate);

    // this will turn off the DotStar/RGB LED and ensure proper transition to Run
    SetMode(GunMode_Run);
}

void startIrCamTimer(int frequencyHz)
{
#if defined(ELITE_SAMD21)
    startTimerEx(&TC4->COUNT16, GCLK_CLKCTRL_ID_TC4_TC5, TC4_IRQn, frequencyHz);
#elif defined(ELITE_SAMD51)
    startTimerEx(&TC3->COUNT16, TC3_GCLK_ID, TC3_IRQn, frequencyHz);
#elif defined(ELITE_ATMEGA32U4)
    startTimer3(frequencyHz);
#elif defined(ELITE_RP2040)
    rp2040EnablePWMTimer(0, frequencyHz);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, rp2040pwmIrq);
    irq_set_enabled(PWM_IRQ_WRAP, true);
#endif
}

#if defined(ELITE_SAMD21)
void startTimerEx(TcCount16* ptc, uint16_t gclkCtrlId, IRQn_Type irqn, int frequencyHz)
{
    // use Generic clock generator 0
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN_GCLK0 | gclkCtrlId);
    while(GCLK->STATUS.bit.SYNCBUSY == 1); // wait for sync
    
    ptc->CTRLA.bit.ENABLE = 0;
    while(ptc->STATUS.bit.SYNCBUSY == 1); // wait for sync
    
    // Use the 16-bit timer
    ptc->CTRLA.reg |= TC_CTRLA_MODE_COUNT16;
    while(ptc->STATUS.bit.SYNCBUSY == 1); // wait for sync
    
    // Use match mode so that the timer counter resets when the count matches the compare register
    ptc->CTRLA.reg |= TC_CTRLA_WAVEGEN_MFRQ;
    while(ptc->STATUS.bit.SYNCBUSY == 1); // wait for sync
    
    // Set prescaler
    ptc->CTRLA.reg |= TIMER_TC_CTRLA_PRESCALER_DIV;
    while(ptc->STATUS.bit.SYNCBUSY == 1); // wait for sync
    
    setTimerFrequency(ptc, frequencyHz);
    
    // Enable the compare interrupt
    ptc->INTENSET.reg = 0;
    ptc->INTENSET.bit.MC0 = 1;
    
    NVIC_EnableIRQ(irqn);
    
    ptc->CTRLA.bit.ENABLE = 1;
    while(ptc->STATUS.bit.SYNCBUSY == 1); // wait for sync
}

void TC4_Handler()
{
    // If this interrupt is due to the compare register matching the timer count
    if(TC4->COUNT16.INTFLAG.bit.MC0 == 1) {
        // clear interrupt
        TC4->COUNT16.INTFLAG.bit.MC0 = 1;

        irPosUpdateTick = 1;
    }
}
#endif // ELITE_SAMD21

#if defined(ELITE_SAMD51)
void startTimerEx(TcCount16* ptc, uint16_t gclkCtrlId, IRQn_Type irqn, int frequencyHz)
{
    // use Generic clock generator 0
    GCLK->PCHCTRL[gclkCtrlId].reg = GCLK_PCHCTRL_GEN_GCLK0 | GCLK_PCHCTRL_CHEN;
    while(GCLK->SYNCBUSY.reg); // wait for sync
    
    ptc->CTRLA.bit.ENABLE = 0;
    while(ptc->SYNCBUSY.bit.STATUS == 1); // wait for sync
    
    // Use the 16-bit timer
    ptc->CTRLA.reg |= TC_CTRLA_MODE_COUNT16;
    while(ptc->SYNCBUSY.bit.STATUS == 1); // wait for sync
    
    // Use match mode so that the timer counter resets when the count matches the compare register
    ptc->WAVE.bit.WAVEGEN = TC_WAVE_WAVEGEN_MFRQ;
    while(ptc->SYNCBUSY.bit.STATUS == 1); // wait for sync
    
    // Set prescaler
    ptc->CTRLA.reg |= TIMER_TC_CTRLA_PRESCALER_DIV;
    while(ptc->SYNCBUSY.bit.STATUS == 1); // wait for sync
    
    setTimerFrequency(ptc, frequencyHz);
    
    // Enable the compare interrupt
    ptc->INTENSET.reg = 0;
    ptc->INTENSET.bit.MC0 = 1;
    
    NVIC_EnableIRQ(irqn);
    
    ptc->CTRLA.bit.ENABLE = 1;
    while(ptc->SYNCBUSY.bit.STATUS == 1); // wait for sync
}

void TC3_Handler()
{
    // If this interrupt is due to the compare register matching the timer count
    if(TC3->COUNT16.INTFLAG.bit.MC0 == 1) {
        // clear interrupt
        TC3->COUNT16.INTFLAG.bit.MC0 = 1;

        irPosUpdateTick = 1;
    }
}
#endif // ELITE_SAMD51

#if defined(ELITE_SAMD21) || defined(ELITE_SAMD51)
void setTimerFrequency(TcCount16* ptc, int frequencyHz)
{
    int compareValue = (F_CPU / (TIMER_PRESCALER_DIV * frequencyHz));

    // Make sure the count is in a proportional position to where it was
    // to prevent any jitter or disconnect when changing the compare value.
    ptc->COUNT.reg = map(ptc->COUNT.reg, 0, ptc->CC[0].reg, 0, compareValue);
    ptc->CC[0].reg = compareValue;

#if defined(ELITE_SAMD21)
    while(ptc->STATUS.bit.SYNCBUSY == 1);
#elif defined(ELITE_SAMD51)
    while(ptc->SYNCBUSY.bit.STATUS == 1);
#endif
}
#endif

#ifdef ELITE_ATMEGA32U4
void startTimer3(unsigned long frequencyHz)
{
    // disable comapre output mode
    TCCR3A = 0;
    
    //set the pre-scalar to 8 and set Clear on Compare
    TCCR3B = (1 << CS31) | (1 << WGM32); 
    
    // set compare value
    OCR3A = F_CPU / (8UL * frequencyHz);
    
    // enable Timer 3 Compare A interrupt
    TIMSK3 = 1 << OCIE3A;
}

// Timer3 compare A interrupt
ISR(TIMER3_COMPA_vect)
{
    irPosUpdateTick = 1;
}
#endif // ELITE_ATMEGA32U4

#ifdef ARDUINO_ARCH_RP2040
void rp2040EnablePWMTimer(unsigned int slice_num, unsigned int frequency)
{
    pwm_config pwmcfg = pwm_get_default_config();
    float clkdiv = (float)clock_get_hz(clk_sys) / (float)(65535 * frequency);
    if(clkdiv < 1.0f) {
        clkdiv = 1.0f;
    } else {
        // really just need to round up 1 lsb
        clkdiv += 2.0f / (float)(1u << PWM_CH1_DIV_INT_LSB);
    }
    
    // set the clock divider in the config and fetch the actual value that is used
    pwm_config_set_clkdiv(&pwmcfg, clkdiv);
    clkdiv = (float)pwmcfg.div / (float)(1u << PWM_CH1_DIV_INT_LSB);
    
    // calculate wrap value that will trigger the IRQ for the target frequency
    pwm_config_set_wrap(&pwmcfg, (float)clock_get_hz(clk_sys) / (frequency * clkdiv));
    
    // initialize and start the slice and enable IRQ
    pwm_init(slice_num, &pwmcfg, true);
    pwm_set_irq_enabled(slice_num, true);
}

void rp2040pwmIrq(void)
{
    pwm_hw->intr = 0xff;
    irPosUpdateTick = 1;
}
#endif

#ifdef ELITE_NO_HW_TIMER
void NoHardwareTimerCamTickMicros()
{
    unsigned long us = micros();
    if(us - irPosUpdateTime >= 1000000UL / IRCamUpdateRate) {
        irPosUpdateTime = us;
        irPosUpdateTick = 1;
    }
}

void NoHardwareTimerCamTickMillis()
{
    unsigned long ms = millis();
    if(ms - irPosUpdateTime >= (1000UL + (IRCamUpdateRate / 2)) / IRCamUpdateRate) {
        irPosUpdateTime = ms;
        irPosUpdateTick = 1;
    }
}
#endif // ELITE_NO_HW_TIMER

void loop() {
    ELITE_NO_HW_TIMER_UPDATE();
    
    // poll/update button states with 1ms interval so debounce mask is more effective
    buttons.Poll(1);
    buttons.Repeat();

    switch(gunMode) {
        case GunMode_Pause:
            if(buttons.pressedReleased == ExitPauseModeBtnMask) {
                SetMode(GunMode_Run);
            } else if(buttons.pressedReleased == BtnMask_Trigger) {
                SetMode(GunMode_CalCenter);
            } else if(buttons.pressedReleased == RunModeNormalBtnMask) {
                SetRunMode(RunMode_Normal);
            } else if(buttons.pressedReleased == RunModeAverageBtnMask) {
                SetRunMode(runMode == RunMode_Average ? RunMode_Average2 : RunMode_Average);
            } else if(buttons.pressedReleased == RunModeProcessingBtnMask) {
                SetRunMode(RunMode_Processing);
            } else if(buttons.pressedReleased == IRSensitivityUpBtnMask) {
                IncreaseIrSensitivity();
            } else if(buttons.pressedReleased == IRSensitivityDownBtnMask) {
                DecreaseIrSensitivity();
            } else if(buttons.pressedReleased == SaveBtnMask) {
                SavePreferences();
            } else {
                SelectCalProfileFromBtnMask(buttons.pressedReleased);
            }

            PrintResults();
            
            break;
        case GunMode_CalCenter:
            AbsMouse5.move(MouseMaxX / 2, MouseMaxY / 2);
            if(buttons.pressedReleased & CancelCalBtnMask) {
                CancelCalibration();
            } else if(buttons.pressedReleased == SkipCalCenterBtnMask) {
                Serial.println("Calibrate Center skipped");
                SetMode(GunMode_CalVert);
            } else if(buttons.pressed & BtnMask_Trigger) {
                // trigger pressed, begin center cal 
                CalCenter();
                // extra delay to wait for trigger to release (though not required)
                SetModeWaitNoButtons(GunMode_CalVert, 500);
            }
            break;
        case GunMode_CalVert:
            if(buttons.pressedReleased & CancelCalBtnMask) {
                CancelCalibration();
            } else {
                if(buttons.pressed & BtnMask_Trigger) {
                    SetMode(GunMode_CalHoriz);
                } else {
                    CalVert();
                }
            }
            break;
        case GunMode_CalHoriz:
            if(buttons.pressedReleased & CancelCalBtnMask) {
                CancelCalibration();
            } else {
                if(buttons.pressed & BtnMask_Trigger) {
                    ApplyCalToProfile();
                    SetMode(GunMode_Run);
                } else {
                    CalHoriz();
                }
            }
            break;
        default:
            /* ---------------------- LET'S GO --------------------------- */
            switch(runMode) {
            case RunMode_Processing:
                ExecRunModeProcessing();
                break;
            case RunMode_Average:
            case RunMode_Average2:
            case RunMode_Normal:
            default:
                ExecRunMode();
                break;
            }
            break;
    }
#ifdef DEBUG_SERIAL
    PrintDebugSerial();
#endif // DEBUG_SERIAL
}

/*        -----------------------------------------------        */
/* --------------------------- METHODS ------------------------- */
/*        -----------------------------------------------        */

void ExecRunMode()
{
#ifdef DEBUG_SERIAL
    Serial.print("exec run mode ");
    Serial.println(RunModeLabels[runMode]);
#endif
    moveIndex = 0;
    buttons.ReportEnable();
    for(;;) {
        buttons.Poll(0);

        ELITE_NO_HW_TIMER_UPDATE();
        if(irPosUpdateTick) {
            irPosUpdateTick = 0;
            GetPosition();
            
            int halfHscale = (int)(myElite.h() * xScale + 0.5f) / 2;
            moveXAxis = map(finalX, xCenter + halfHscale, xCenter - halfHscale, 0, MouseMaxX);
            halfHscale = (int)(myElite.h() * yScale + 0.5f) / 2;
            moveYAxis = map(finalY, yCenter + halfHscale, yCenter - halfHscale, 0, MouseMaxY);

            switch(runMode) {
            case RunMode_Average:
                // 2 position moving average
                moveIndex ^= 1;
                moveXAxisArr[moveIndex] = moveXAxis;
                moveYAxisArr[moveIndex] = moveYAxis;
                moveXAxis = (moveXAxisArr[0] + moveXAxisArr[1]) / 2;
                moveYAxis = (moveYAxisArr[0] + moveYAxisArr[1]) / 2;
                break;
            case RunMode_Average2:
                // weighted average of current position and previous 2
                if(moveIndex < 2) {
                    ++moveIndex;
                } else {
                    moveIndex = 0;
                }
                moveXAxisArr[moveIndex] = moveXAxis;
                moveYAxisArr[moveIndex] = moveYAxis;
                moveXAxis = (moveXAxis + moveXAxisArr[0] + moveXAxisArr[1] + moveXAxisArr[1] + 2) / 4;
                moveYAxis = (moveYAxis + moveYAxisArr[0] + moveYAxisArr[1] + moveYAxisArr[1] + 2) / 4;
                break;
            case RunMode_Normal:
            default:
                break;
            }

            conMoveXAxis = constrain(moveXAxis, 0, MouseMaxX);
            conMoveYAxis = constrain(moveYAxis, 0, MouseMaxY);                
            AbsMouse5.move(conMoveXAxis, conMoveYAxis);
            
#ifdef DEBUG_SERIAL
            ++irPosCount;
#endif // DEBUG_SERIAL
        }

        if(buttons.pressedReleased == EnterPauseModeBtnMask) {
            SetMode(GunMode_Pause);
            buttons.ReportDisable();
            return;
        }

#ifdef DEBUG_SERIAL
        ++frameCount;
        PrintDebugSerial();
#endif // DEBUG_SERIAL
    }
}

// from Elite_4IR_Test_BETA sketch
// for use with the Elite_4IR_Processing_Sketch_BETA Processing sketch
void ExecRunModeProcessing()
{
    // constant offset added to output values
    const int processingOffset = 100;

    buttons.ReportDisable();
    for(;;) {
        buttons.Poll(1);
        if(buttons.pressedReleased & EnterPauseModeProcessingBtnMask) {
            SetMode(GunMode_Pause);
            return;
        }

        ELITE_NO_HW_TIMER_UPDATE();
        if(irPosUpdateTick) {
            irPosUpdateTick = 0;
        
            int error = dfrIRPos.basicAtomic(DFRobotIRPositionEx::Retry_2);
            if(error == DFRobotIRPositionEx::Error_Success) {
                myElite.begin(dfrIRPos.xPositions(), dfrIRPos.yPositions(), dfrIRPos.seen(), MouseMaxX / 2, MouseMaxY / 2);
                UpdateLastSeen();
                for(int i = 0; i < 4; i++) {
                    Serial.print(map(myElite.testX(i), 0, MouseMaxX, CamMaxX, 0) + processingOffset);
                    Serial.print(",");
                    Serial.print(map(myElite.testY(i), 0, MouseMaxY, CamMaxY, 0) + processingOffset);
                    Serial.print(",");
                }
                Serial.print(map(myElite.x(), 0, MouseMaxX, CamMaxX, 0) + processingOffset);
                Serial.print(",");
                Serial.print(map(myElite.y(), 0, MouseMaxY, CamMaxY, 0) + processingOffset);
                Serial.print(",");
                Serial.print(map(myElite.testMedianX(), 0, MouseMaxX, CamMaxX, 0) + processingOffset);
                Serial.print(",");
                Serial.println(map(myElite.testMedianY(), 0, MouseMaxY, CamMaxY, 0) + processingOffset);
            } else if(error == DFRobotIRPositionEx::Error_IICerror) {
                Serial.println("Device not available!");
            }
        }
    }
}

// center calibration with a bit of averaging
void CalCenter()
{
    unsigned int xAcc = 0;
    unsigned int yAcc = 0;
    unsigned int count = 0;
    unsigned long ms = millis();
    
    // accumulate center position over a bit of time for some averaging
    while(millis() - ms < 333) {
        // center pointer
        AbsMouse5.move(MouseMaxX / 2, MouseMaxY / 2);
        
        // get position
        if(GetPositionIfReady()) {
            xAcc += finalX;
            yAcc += finalY;
            count++;
            
            xCenter = finalX;
            yCenter = finalY;
            PrintCalInterval();
        }

        // poll buttons
        buttons.Poll(1);
        
        // if trigger not pressed then break out of loop early
        if(!(buttons.debounced & BtnMask_Trigger)) {
            break;
        }
    }

    // unexpected, but make sure x and y positions are accumulated
    if(count) {
        xCenter = xAcc / count;
        yCenter = yAcc / count;
    } else {
        Serial.print("Unexpected Center calibration failure, no center position was acquired!");
        // just continue anyway
    }

    PrintCalInterval();
}

// vertical calibration 
void CalVert()
{
    if(GetPositionIfReady()) {
        int halfH = (int)(myElite.h() * yScale + 0.5f) / 2;
        moveYAxis = map(finalY, yCenter + halfH, yCenter - halfH, 0, MouseMaxY);
        conMoveXAxis = MouseMaxX / 2;
        conMoveYAxis = constrain(moveYAxis, 0, MouseMaxY);
        AbsMouse5.move(conMoveXAxis, conMoveYAxis);
    }
    
    if(buttons.repeat & BtnMask_B) {
        yScale = yScale + ScaleStep;
    }
    
    if(buttons.repeat & BtnMask_A) {
        if(yScale > 0.005f) {
            yScale = yScale - ScaleStep;
        }
    }

    if(buttons.pressedReleased == BtnMask_Start) {
        yCenter--;
    } else if(buttons.pressedReleased == BtnMask_Select) {
        yCenter++;
    }
    
    PrintCalInterval();
}

// horizontal calibration 
void CalHoriz()
{
    if(GetPositionIfReady()) {    
        int halfH = (int)(myElite.h() * xScale + 0.5f) / 2;
        moveXAxis = map(finalX, xCenter + halfH, xCenter - halfH, 0, MouseMaxX);
        conMoveXAxis = constrain(moveXAxis, 0, MouseMaxX);
        conMoveYAxis = MouseMaxY / 2;
        AbsMouse5.move(conMoveXAxis, conMoveYAxis);
    }

    if(buttons.repeat & BtnMask_B) {
        xScale = xScale + ScaleStep;
    }
    
    if(buttons.repeat & BtnMask_A) {
        if(xScale > 0.005f) {
            xScale = xScale - ScaleStep;
        }
    }
    
    if(buttons.pressedReleased == BtnMask_Start) {
        xCenter--;
    } else if(buttons.pressedReleased == BtnMask_Select) {
        xCenter++;
    }

    PrintCalInterval();
}

// Helper to get position if the update tick is set
bool GetPositionIfReady()
{
    if(irPosUpdateTick) {
        irPosUpdateTick = 0;
        GetPosition();
        return true;
    }
    return false;
}

// Get tilt adjusted position from IR postioning camera
// Updates finalX and finalY values
void GetPosition()
{
    int error = dfrIRPos.basicAtomic(DFRobotIRPositionEx::Retry_2);
    if(error == DFRobotIRPositionEx::Error_Success) {
        myElite.begin(dfrIRPos.xPositions(), dfrIRPos.yPositions(), dfrIRPos.seen(), xCenter, yCenter);
#ifdef EXTRA_POS_GLITCH_FILTER
        if((abs(myElite.X() - finalX) > BadMoveThreshold || abs(myElite.Y() - finalY) > BadMoveThreshold) && badFinalTick < BadMoveCountThreshold) {
            ++badFinalTick;
        } else {
            if(badFinalTick) {
                badFinalCount++;
                badFinalTick = 0;
            }
            finalX = myElite.X();
            finalY = myElite.Y();
        }
#else
        finalX = myElite.x();
        finalY = myElite.y();
#endif // EXTRA_POS_GLITCH_FILTER

        UpdateLastSeen();
#if DEBUG_SERIAL == 2
        Serial.print(finalX);
        Serial.print(' ');
        Serial.print(finalY);
        Serial.print("   ");
        Serial.println(myElite.h());
#endif
    } else if(error != DFRobotIRPositionEx::Error_DataMismatch) {
        Serial.println("Device not available!");
    }
}

// wait up to given amount of time for no buttons to be pressed before setting the mode
void SetModeWaitNoButtons(GunMode_e newMode, unsigned long maxWait)
{
    unsigned long ms = millis();
    while(buttons.debounced && (millis() - ms < maxWait)) {
        buttons.Poll(1);
    }
    SetMode(newMode);
}

// update the last seen value
// only to be called during run mode since this will modify the LED colour
void UpdateLastSeen() {
    if(lastSeen != myElite.seen()) {
        if(!lastSeen && myElite.seen()) {
            LedOff();
        } else if(lastSeen && !myElite.seen()) {
            SetLedPackedColor(IRSeen0Color);
        }
        lastSeen = myElite.seen();
    }
}

void SetMode(GunMode_e newMode)
{
    if(gunMode == newMode) {
        return;
    }
    
    // exit current mode
    switch(gunMode) {
    case GunMode_Run:
        stateFlags |= StateFlag_PrintPreferences;
        break;
    case GunMode_CalHoriz:
        break;
    case GunMode_CalVert:
        break;
    case GunMode_CalCenter:
        break;
    case GunMode_Pause:
        break;
    }
    
    // enter new mode
    gunMode = newMode;
    switch(newMode) {
    case GunMode_Run:
        // begin run mode with all 4 points seen
        lastSeen = 0x0F;        
        break;
    case GunMode_CalHoriz:
        break;
    case GunMode_CalVert:
        break;
    case GunMode_CalCenter:
        break;
    case GunMode_Pause:
        stateFlags |= StateFlag_SavePreferencesEn | StateFlag_PrintSelectedProfile;
        break;
    }

    SetLedColorFromMode();
}

// set new run mode and apply it to the selected profile
void SetRunMode(RunMode_e newMode)
{
    if(newMode >= RunMode_Count) {
        return;
    }

    // block Processing/test modes being applied to a profile
    if(newMode <= RunMode_ProfileMax && profileData[selectedProfile].runMode != newMode) {
        profileData[selectedProfile].runMode = newMode;
        stateFlags |= StateFlag_SavePreferencesEn;
    }
    
    if(runMode != newMode) {
        runMode = newMode;
        if(!(stateFlags & StateFlag_PrintSelectedProfile)) {
            PrintRunMode();
        }
    }
}

void PrintResults()
{
    if(millis() - lastPrintMillis < 100) {
        return;
    }

    if(!Serial.dtr()) {
        stateFlags |= StateFlagsDtrReset;
        return;
    }

    PrintPreferences();
    /*
    Serial.print(finalX);
    Serial.print(" (");
    Serial.print(MoveXAxis);
    Serial.print("), ");
    Serial.print(finalY);
    Serial.print(" (");
    Serial.print(MoveYAxis);
    Serial.print("), H ");
    Serial.println(myElite.H());*/

    //Serial.print("conMove ");
    //Serial.print(conMoveXAxis);
    //Serial.println(conMoveYAxis);
    
    if(stateFlags & StateFlag_PrintSelectedProfile) {
        stateFlags &= ~StateFlag_PrintSelectedProfile;
        PrintSelectedProfile();
        PrintIrSensitivity();
        PrintRunMode();
        PrintCal();
    }
        
    lastPrintMillis = millis();
}

void PrintCalInterval()
{
    if(millis() - lastPrintMillis < 100) {
        return;
    }
    PrintCal();
    lastPrintMillis = millis();
}

void PrintCal()
{
    Serial.print("Calibration: Center x,y: ");
    Serial.print(xCenter);
    Serial.print(",");
    Serial.print(yCenter);
    Serial.print(" Scale x,y: ");
    Serial.print(xScale, 3);
    Serial.print(",");
    Serial.println(yScale, 3);
}

void PrintRunMode()
{
    if(runMode < RunMode_Count) {
        Serial.print("Mode: ");
        Serial.println(RunModeLabels[runMode]);
    }
}

// helper in case this changes
float CalScalePrefToFloat(uint16_t scale)
{
    return (float)scale / 1000.0f;
}

// helper in case this changes
uint16_t CalScaleFloatToPref(float scale)
{
    return (uint16_t)(scale * 1000.0f);
}

void PrintPreferences()
{
    if(!(stateFlags & StateFlag_PrintPreferences) || !Serial.dtr()) {
        return;
    }

    stateFlags &= ~StateFlag_PrintPreferences;
    
    PrintNVPrefsError();

    if(stateFlags & StateFlag_PrintPreferencesStorage) {
        stateFlags &= ~StateFlag_PrintPreferencesStorage;
        PrintNVStorage();
    }
    
    Serial.print("Default Profile: ");
    Serial.println(profileDesc[ElitePreferences::preferences.profile].profileLabel);
    
    Serial.println("Profiles:");
    for(unsigned int i = 0; i < ElitePreferences::preferences.profileCount; ++i) {
        // report if a profile has been cal'd
        if(profileData[i].xCenter && profileData[i].yCenter) {
            size_t len = strlen(profileDesc[i].buttonLabel) + 2;
            Serial.print(profileDesc[i].buttonLabel);
            Serial.print(": ");
            if(profileDesc[i].profileLabel && profileDesc[i].profileLabel[0]) {
                Serial.print(profileDesc[i].profileLabel);
                len += strlen(profileDesc[i].profileLabel);
            }
            while(len < 26) {
                Serial.print(' ');
                ++len;
            }
            Serial.print("Center: ");
            Serial.print(profileData[i].xCenter);
            Serial.print(",");
            Serial.print(profileData[i].yCenter);
            Serial.print(" Scale: ");
            Serial.print(CalScalePrefToFloat(profileData[i].xScale), 3);
            Serial.print(",");
            Serial.print(CalScalePrefToFloat(profileData[i].yScale), 3);
            Serial.print(" IR: ");
            Serial.print((unsigned int)profileData[i].irSensitivity);
            Serial.print(" Mode: ");
            Serial.println((unsigned int)profileData[i].runMode);
        }
    }
}

void PrintNVStorage()
{
#ifdef ELITE_FLASH_ENABLE
    unsigned int required = ElitePreferences::Size();
#ifndef PRINT_VERBOSE
    if(required < flash.size()) {
        return;
    }
#endif
    Serial.print("NV Storage capacity: ");
    Serial.print(flash.size());
    Serial.print(", required size: ");
    Serial.println(required);
#ifdef PRINT_VERBOSE
    Serial.print("Profile struct size: ");
    Serial.print((unsigned int)sizeof(ElitePreferences::ProfileData_t));
    Serial.print(", Profile data array size: ");
    Serial.println((unsigned int)sizeof(profileData));
#endif
#endif // ELITE_FLASH_ENABLE
}

void PrintNVPrefsError()
{
    if(nvPrefsError != ElitePreferences::Error_Success) {
        Serial.print(NVRAMlabel);
        Serial.print(" error: ");
#ifdef ELITE_FLASH_ENABLE
        Serial.println(ElitePreferences::ErrorCodeToString(nvPrefsError));
#else
        Serial.println(nvPrefsError);
#endif // ELITE_FLASH_ENABLE
    }
}

void LoadPreferences()
{
    if(!nvAvailable) {
        return;
    }

#ifdef ELITE_FLASH_ENABLE
    nvPrefsError = ElitePreferences::Load(flash);
#else
    nvPrefsError = elitePreferences.Load();
#endif // ELITE_FLASH_ENABLE
    VerifyPreferences();
}

void VerifyPreferences()
{
    // center 0 is used as "no cal data"
    for(unsigned int i = 0; i < ProfileCount; ++i) {
        if(profileData[i].xCenter >= MouseMaxX || profileData[i].yCenter >= MouseMaxY || profileData[i].xScale == 0 || profileData[i].yScale == 0) {
            profileData[i].xCenter = 0;
            profileData[i].yCenter = 0;
        }

        // if the scale values are large, assign 0 so the values will be ignored
        if(profileData[i].xScale >= 30000) {
            profileData[i].xScale = 0;
        }
        if(profileData[i].yScale >= 30000) {
            profileData[i].yScale = 0;
        }
    
        if(profileData[i].irSensitivity > DFRobotIRPositionEx::Sensitivity_Max) {
            profileData[i].irSensitivity = DFRobotIRPositionEx::Sensitivity_Default;
        }

        if(profileData[i].runMode >= RunMode_Count) {
            profileData[i].runMode = RunMode_Normal;
        }
    }

    // if default profile is not valid, use current selected profile instead
    if(ElitePreferences::preferences.profile >= ProfileCount) {
        ElitePreferences::preferences.profile = (uint8_t)selectedProfile;
    }
}

// Apply initial preferences, intended to be called only in setup() after LoadPreferences()
// this will apply the preferences data as the initial values
void ApplyInitialPrefs()
{   
    // if default profile is valid then use it
    if(ElitePreferences::preferences.profile < ProfileCount) {
        // note, just set the value here not call the function to do the set
        selectedProfile = ElitePreferences::preferences.profile;

        // set the current IR camera sensitivity
        if(profileData[selectedProfile].irSensitivity <= DFRobotIRPositionEx::Sensitivity_Max) {
            irSensitivity = (DFRobotIRPositionEx::Sensitivity_e)profileData[selectedProfile].irSensitivity;
        }

        // set the run mode
        if(profileData[selectedProfile].runMode < RunMode_Count) {
            runMode = (RunMode_e)profileData[selectedProfile].runMode;
        }
    }
}

void SavePreferences()
{
    if(!nvAvailable || !(stateFlags & StateFlag_SavePreferencesEn)) {
        return;
    }

    // Only allow one write per pause state until something changes.
    // Extra protection to ensure the same data can't write a bunch of times.
    stateFlags &= ~StateFlag_SavePreferencesEn;
    
    // use selected profile as the default
    ElitePreferences::preferences.profile = (uint8_t)selectedProfile;

#ifdef ELITE_FLASH_ENABLE
    nvPrefsError = ElitePreferences::Save(flash);
#else
    nvPrefsError = ElitePreferences::Save();
#endif // ELITE_FLASH_ENABLE
    if(nvPrefsError == ElitePreferences::Error_Success) {
        Serial.print("Settings saved to ");
        Serial.println(NVRAMlabel);
    } else {
        Serial.println("Error saving Preferences.");
        PrintNVPrefsError();
    }
}

void SelectCalProfileFromBtnMask(uint32_t mask)
{
    // only check if buttons are set in the mask
    if(!mask) {
        return;
    }
    for(unsigned int i = 0; i < ProfileCount; ++i) {
        if(profileDesc[i].buttonMask == mask) {
            SelectCalProfile(i);
            return;
        }
    }
}

void CycleIrSensitivity()
{
    uint8_t sens = irSensitivity;
    if(irSensitivity < DFRobotIRPositionEx::Sensitivity_Max) {
        sens++;
    } else {
        sens = DFRobotIRPositionEx::Sensitivity_Min;
    }
    SetIrSensitivity(sens);
}

void IncreaseIrSensitivity()
{
    uint8_t sens = irSensitivity;
    if(irSensitivity < DFRobotIRPositionEx::Sensitivity_Max) {
        sens++;
        SetIrSensitivity(sens);
    }
}

void DecreaseIrSensitivity()
{
    uint8_t sens = irSensitivity;
    if(irSensitivity > DFRobotIRPositionEx::Sensitivity_Min) {
        sens--;
        SetIrSensitivity(sens);
    }
}

// set a new IR camera sensitivity and apply to the selected profile
void SetIrSensitivity(uint8_t sensitivity)
{
    if(sensitivity > DFRobotIRPositionEx::Sensitivity_Max) {
        return;
    }

    if(profileData[selectedProfile].irSensitivity != sensitivity) {
        profileData[selectedProfile].irSensitivity = sensitivity;
        stateFlags |= StateFlag_SavePreferencesEn;
    }

    if(irSensitivity != (DFRobotIRPositionEx::Sensitivity_e)sensitivity) {
        irSensitivity = (DFRobotIRPositionEx::Sensitivity_e)sensitivity;
        dfrIRPos.sensitivityLevel(irSensitivity);
        if(!(stateFlags & StateFlag_PrintSelectedProfile)) {
            PrintIrSensitivity();
        }
    }
}

void PrintIrSensitivity()
{
    Serial.print("IR Camera Sensitivity: ");
    Serial.println((int)irSensitivity);
}

void CancelCalibration()
{
    Serial.println("Calibration cancelled");
    // re-print the profile
    stateFlags |= StateFlag_PrintSelectedProfile;
    // re-apply the cal stored in the profile
    RevertToCalProfile(selectedProfile);
    // return to pause mode
    SetMode(GunMode_Pause);
}

void PrintSelectedProfile()
{
    Serial.print("Profile: ");
    Serial.println(profileDesc[selectedProfile].profileLabel);
}

// select a profile
bool SelectCalProfile(unsigned int profile)
{
    if(profile >= ProfileCount) {
        return false;
    }

    if(selectedProfile != profile) {
        stateFlags |= StateFlag_PrintSelectedProfile;
        selectedProfile = profile;
    }

    bool valid = SelectCalPrefs(profile);

    // set IR sensitivity
    if(profileData[profile].irSensitivity <= DFRobotIRPositionEx::Sensitivity_Max) {
        SetIrSensitivity(profileData[profile].irSensitivity);
    }

    // set run mode
    if(profileData[profile].runMode < RunMode_Count) {
        SetRunMode((RunMode_e)profileData[profile].runMode);
    }

    SetLedColorFromMode();

    // enable save to allow setting new default profile
    stateFlags |= StateFlag_SavePreferencesEn;
    return valid;
}

bool SelectCalPrefs(unsigned int profile)
{
    if(profile >= ProfileCount) {
        return false;
    }

    // if center values are set, assume profile is populated
    if(profileData[profile].xCenter && profileData[profile].yCenter) {
        xCenter = profileData[profile].xCenter;
        yCenter = profileData[profile].yCenter;
        
        // 0 scale will be ignored
        if(profileData[profile].xScale) {
            xScale = CalScalePrefToFloat(profileData[profile].xScale);
        }
        if(profileData[profile].yScale) {
            yScale = CalScalePrefToFloat(profileData[profile].yScale);
        }
        return true;
    }
    return false;
}

// revert back to useable settings, even if not cal'd
void RevertToCalProfile(unsigned int profile)
{
    // if the current profile isn't valid
    if(!SelectCalProfile(profile)) {
        // revert to settings from any valid profile
        for(unsigned int i = 0; i < ProfileCount; ++i) {
            if(SelectCalProfile(i)) {
                break;
            }
        }

        // stay selected on the specified profile
        SelectCalProfile(profile);
    }
}

// apply current cal to the selected profile
void ApplyCalToProfile()
{
    profileData[selectedProfile].xCenter = xCenter;
    profileData[selectedProfile].yCenter = yCenter;
    profileData[selectedProfile].xScale = CalScaleFloatToPref(xScale);
    profileData[selectedProfile].yScale = CalScaleFloatToPref(yScale);

    stateFlags |= StateFlag_PrintSelectedProfile;
}

void SetLedPackedColor(uint32_t color)
{
#ifdef DOTSTAR_ENABLE
    dotstar.setPixelColor(0, Adafruit_DotStar::gamma32(color & 0x00FFFFFF));
    dotstar.show();
#endif // DOTSTAR_ENABLE
#ifdef NEOPIXEL_PIN
    neopixel.setPixelColor(0, Adafruit_NeoPixel::gamma32(color & 0x00FFFFFF));
    neopixel.show();
#endif // NEOPIXEL_PIN
}

void LedOff()
{
#ifdef DOTSTAR_ENABLE
    dotstar.setPixelColor(0, 0);
    dotstar.show();
#endif // DOTSTAR_ENABLE
#ifdef NEOPIXEL_PIN
    neopixel.setPixelColor(0, 0);
    neopixel.show();
#endif // NEOPIXEL_PIN
}

void SetLedColorFromMode()
{
    switch(gunMode) {
    case GunMode_CalHoriz:
    case GunMode_CalVert:
    case GunMode_CalCenter:
        SetLedPackedColor(CalModeColor);
        break;
    case GunMode_Pause:
        SetLedPackedColor(profileDesc[selectedProfile].color);
        break;
    case GunMode_Run:
        if(lastSeen) {
            LedOff();
        } else {
            SetLedPackedColor(IRSeen0Color);
        }
        break;
    default:
        break;
    }
}

#ifdef DEBUG_SERIAL
void PrintDebugSerial()
{
    // only print every second
    if(millis() - serialDbMs >= 1000 && Serial.dtr()) {
#ifdef EXTRA_POS_GLITCH_FILTER
        Serial.print("bad final count ");
        Serial.print(badFinalCount);
        Serial.print(", bad move count ");
        Serial.println(badMoveCount);
#endif // EXTRA_POS_GLITCH_FILTER
        Serial.print("mode ");
        Serial.print(gunMode);
        Serial.print(", IR pos fps ");
        Serial.print(irPosCount);
        Serial.print(", loop/sec ");
        Serial.print(frameCount);

        Serial.print(", Mouse X,Y ");
        Serial.print(conMoveXAxis);
        Serial.print(",");
        Serial.println(conMoveYAxis);
        
        frameCount = 0;
        irPosCount = 0;
        serialDbMs = millis();
    }
}
#endif // DEBUG_SERIAL
