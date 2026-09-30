#include <ControllerHaptics.h>
#include "streaming/session.h"

#include <Limelight.h>
#include "SDL_compat.h"
#include "settings/mappingmanager.h"
#include "streaming/input/dualsensehaptics.h"
#include "streaming/audio/dualsenseaudio.h"
#include "streaming/audio/miccapture.h"
#include "streaming/audio/renderers/sdl.h"

#include <QtMath>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <cstring>
#include <cmath>

// How long the Start button must be pressed to toggle mouse emulation
#define MOUSE_EMULATION_LONG_PRESS_TIME 750

// How long between polling the gamepad to send virtual mouse input
#define MOUSE_EMULATION_POLLING_INTERVAL 50

// Determines how fast the mouse will move each interval
#define MOUSE_EMULATION_MOTION_MULTIPLIER 4

// Determines the maximum motion amount before allowing movement
#define MOUSE_EMULATION_DEADZONE 2

// Haptic capabilities (in addition to those from SDL_HapticQuery())
#define ML_HAPTIC_GC_RUMBLE         (1U << 16)
#define ML_HAPTIC_SIMPLE_RUMBLE     (1U << 17)
#define ML_HAPTIC_GC_TRIGGER_RUMBLE (1U << 18)

// Apollo Extended controller-generation request encoded in the legacy
// controller capability field. Older hosts safely ignore these upper bits.
#define APOLLO_EXTENDED_EMULATION_MAGIC 0xEC00
#define APOLLO_EXTENDED_EMULATION_XBOX  1
#define APOLLO_EXTENDED_EMULATION_DS4   2
#define APOLLO_EXTENDED_EMULATION_DS5   3
// The mode field (bits 8-9) overlaps LI_CCAP_DUAL_TOUCHPAD (0x100).
#define APOLLO_EXTENDED_EMULATION_MODE_MASK 0x0300

static uint32_t apolloExtendedEmulationCaps(StreamingPreferences::ControllerEmulationMode emulation,
                                            SDL_GameController* controller, uint8_t type)
{
    int mode = 0;
    if (emulation == StreamingPreferences::CEM_XBOX) mode = APOLLO_EXTENDED_EMULATION_XBOX;
    else if (emulation == StreamingPreferences::CEM_DUALSHOCK4) mode = APOLLO_EXTENDED_EMULATION_DS4;
    else if (emulation == StreamingPreferences::CEM_DUALSENSE) mode = APOLLO_EXTENDED_EMULATION_DS5;
    else if (SDL_GameControllerGetType(controller) == SDL_CONTROLLER_TYPE_PS5) mode = APOLLO_EXTENDED_EMULATION_DS5;
    else if (type == LI_CTYPE_PS) mode = APOLLO_EXTENDED_EMULATION_DS4;
    else if (type == LI_CTYPE_XBOX) mode = APOLLO_EXTENDED_EMULATION_XBOX;
    return APOLLO_EXTENDED_EMULATION_MAGIC | static_cast<uint32_t>(mode << 8);
}

static bool sendDualSenseEffect(SDL_GameController* controller, const uint8_t* effect)
{
    return SDL_GameControllerSendEffect(controller, effect, sizeof(DualSenseOutputReport)) == 0;
}

static bool queryDualSenseHeadset(SDL_GameController* controller)
{
    static const uint8_t query[8] = { 'A', 'R', 'T', 'J', 'A', 'C', 'K', '?' };
    return SDL_GameControllerSendEffect(controller, query, sizeof(query)) == 0;
}

static void activateDualSenseAudio(SDL_GameController* controller,
                                   StreamingPreferences::DualSenseAudioMode mode)
{
    if (mode == StreamingPreferences::DSAM_OFF) {
        return;
    }

    static_assert(sizeof(DualSenseOutputReport) > 43,
                  "DualSense output report must contain the DSP wake fields");

    // SDL takes the native USB payload without report ID 0x02. Consequently,
    // these offsets are one less than the corresponding raw HID report offsets.
    const bool headset = mode == StreamingPreferences::DSAM_USB_HEADSET;
    uint8_t route[sizeof(DualSenseOutputReport)] = {};
    route[0] = 0xf3;
    route[4] = 0xff;
    if (!headset) {
        route[5] = 0xff;
        // The DualSense microphone gain is a separate 0x00-0x40 field.
        // Keep the audio-control byte limited to internal-mic routing: 0xff
        // also enables the microphone attenuation bits and makes USB capture
        // extremely quiet before our software gain is ever applied.
        route[6] = 0x40;
        route[7] = 0x01;
    }

    bool routeOk = sendDualSenseEffect(controller, route);
    if (!headset) {
        SDL_Delay(40);
        routeOk = sendDualSenseEffect(controller, route) && routeOk;
    }

    SDL_Delay(35);
    uint8_t musicRumble[sizeof(DualSenseOutputReport)] = {};
    musicRumble[0] = 0xe0;
    musicRumble[4] = 0x7f;
    musicRumble[5] = headset ? 0x00 : 0xff;
    musicRumble[6] = 0x40;
    musicRumble[7] = headset ? 0x00 : 0x30;
    const bool musicOk = sendDualSenseEffect(controller, musicRumble);

    SDL_Delay(35);
    uint8_t wake[sizeof(DualSenseOutputReport)] = {};
    wake[1] = 0x15;
    wake[38] = 0x03;
    wake[41] = 0x02;
    wake[43] = 0x24;
    const bool wakeOk = sendDualSenseEffect(controller, wake);

    if (!headset) {
        SDL_Delay(35);
        routeOk = sendDualSenseEffect(controller, route) && routeOk;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "DualSense audio activation: mode=%d route=%s music-rumble=%s dsp-wake=%s",
                static_cast<int>(mode),
                routeOk ? "ok" : "failed",
                musicOk ? "ok" : "failed",
                wakeOk ? "ok" : "failed");
}

const int SdlInputHandler::k_ButtonMap[] = {
    A_FLAG, B_FLAG, X_FLAG, Y_FLAG,
    BACK_FLAG, SPECIAL_FLAG, PLAY_FLAG,
    LS_CLK_FLAG, RS_CLK_FLAG,
    LB_FLAG, RB_FLAG,
    UP_FLAG, DOWN_FLAG, LEFT_FLAG, RIGHT_FLAG,
    MISC_FLAG,
    PADDLE1_FLAG, PADDLE2_FLAG, PADDLE3_FLAG, PADDLE4_FLAG,
    TOUCHPAD_FLAG,
};

GamepadState*
SdlInputHandler::findStateForGamepad(SDL_JoystickID id)
{
    int i;

    for (i = 0; i < MAX_GAMEPADS; i++) {
        if (m_GamepadState[i].jsId == id) {
            SDL_assert(!m_MultiController || m_GamepadState[i].index == i);
            return &m_GamepadState[i];
        }
    }

    // We can get a spurious removal event if the device is removed
    // before or during SDL_GameControllerOpen(). This is fine to ignore.
    return nullptr;
}

void SdlInputHandler::sendGamepadState(GamepadState* state)
{
    if (m_ControllerKbmMode) {
        return;
    }
    SDL_assert(m_GamepadMask == 0x1 || m_MultiController);

    // Handle Select+PS as the clickpad button on PS4/5 controllers without a clickpad mapping
    int buttons = state->buttons;
    if (state->clickpadButtonEmulationEnabled) {
        if (state->buttons == (BACK_FLAG | SPECIAL_FLAG)) {
            buttons = MISC_FLAG;
            state->emulatedClickpadButtonDown = true;
        }
        else if (state->emulatedClickpadButtonDown) {
            buttons &= ~MISC_FLAG;
            state->emulatedClickpadButtonDown = false;
        }
    }

    unsigned char lt = state->lt;
    unsigned char rt = state->rt;
    short lsX = state->lsX;
    short lsY = state->lsY;
    short rsX = static_cast<short>(qBound(-32767, static_cast<int>(state->rsX) + state->gyroStickX, 32767));
    short rsY = static_cast<short>(qBound(-32767, static_cast<int>(state->rsY) + state->gyroStickY, 32767));

    // When in single controller mode, merge all gamepad state together
    if (!m_MultiController) {
        for (int i = 0; i < MAX_GAMEPADS; i++) {
            if (m_GamepadState[i].index == state->index) {
                buttons |= m_GamepadState[i].buttons;
                if (lt < m_GamepadState[i].lt) {
                    lt = m_GamepadState[i].lt;
                }
                if (rt < m_GamepadState[i].rt) {
                    rt = m_GamepadState[i].rt;
                }

                // We use abs() here instead of qAbs() for get proper integer promotion to
                // correctly handle abs(-32768), which is not representable in a short.
                if (abs(lsX) < abs(m_GamepadState[i].lsX) || abs(lsY) < abs(m_GamepadState[i].lsY)) {
                    lsX = m_GamepadState[i].lsX;
                    lsY = m_GamepadState[i].lsY;
                }
                if (abs(rsX) < abs(m_GamepadState[i].rsX) || abs(rsY) < abs(m_GamepadState[i].rsY)) {
                    rsX = m_GamepadState[i].rsX;
                    rsY = m_GamepadState[i].rsY;
                }
            }
        }
    }

    LiSendMultiControllerEvent(state->index,
                               m_GamepadMask,
                               buttons,
                               lt,
                               rt,
                               lsX,
                               lsY,
                               rsX,
                               rsY);
}

static QString controllerTypeName(SDL_GameControllerType type)
{
    switch(type){case SDL_CONTROLLER_TYPE_PS5:return QStringLiteral("DualSense");case SDL_CONTROLLER_TYPE_PS4:return QStringLiteral("DualShock 4");case SDL_CONTROLLER_TYPE_XBOXONE:return QStringLiteral("Xbox One");case SDL_CONTROLLER_TYPE_XBOX360:return QStringLiteral("Xbox 360");case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:return QStringLiteral("Nintendo Switch Pro");default:return QStringLiteral("Game controller");}
}

static void publishControllerStatus(GamepadState* state)
{
    if(!state||!state->controller)return;
    QSettings settings;settings.beginGroup(QStringLiteral("livecontrollerstatus/%1").arg(state->jsId));
    const char* name=SDL_GameControllerName(state->controller);settings.setValue(QStringLiteral("name"),QString::fromUtf8(name?name:"Controller"));
    const SDL_GameControllerType type=SDL_GameControllerGetType(state->controller);settings.setValue(QStringLiteral("type"),controllerTypeName(type));
    SDL_Joystick* joystick=SDL_GameControllerGetJoystick(state->controller);const char* path=SDL_JoystickPath(joystick);const bool bluetooth=state->dualSenseBluetooth||(path&&(std::strstr(path,"00001124-0000-1000-8000-00805f9b34fb")||std::strstr(path,"BTH")||std::strstr(path,"Bluetooth")));
    settings.setValue(QStringLiteral("connection"),bluetooth?QStringLiteral("Bluetooth"):QStringLiteral("USB"));settings.setValue(QStringLiteral("battery"),state->batteryPercentage);settings.setValue(QStringLiteral("batteryState"),state->batteryState==1?QStringLiteral("Charging"):state->batteryState==2?QStringLiteral("Discharging"):QStringLiteral("Unknown"));
    settings.setValue(QStringLiteral("headsetConnected"),state->dualSenseHeadsetStateKnown&&state->dualSenseHeadsetConnected);settings.setValue(QStringLiteral("headsetState"),state->dualSenseHeadsetStateKnown?(state->dualSenseHeadsetConnected?QStringLiteral("Connected"):QStringLiteral("Not connected")):QStringLiteral("Unknown"));
    settings.setValue(QStringLiteral("microphone"),type==SDL_CONTROLLER_TYPE_PS5?QStringLiteral("Available"):QStringLiteral("Not available"));settings.endGroup();settings.sync();
}

static void removeControllerStatus(SDL_JoystickID id){QSettings settings;settings.remove(QStringLiteral("livecontrollerstatus/%1").arg(id));settings.sync();}

static float kbmDeadzone(float value)
{
    const float magnitude = qAbs(value);
    return magnitude <= 0.08f ? 0.0f : std::copysign((magnitude - 0.08f) / 0.92f, value);
}

QString SdlInputHandler::controllerKbmSourceForButton(unsigned button) const
{
    static const char* const names[] = {
        "button_a", "button_b", "button_x", "button_y", "button_back", "button_guide",
        "button_start", "button_l3", "button_r3", "button_lb", "button_rb", "dpad_up",
        "dpad_down", "dpad_left", "dpad_right", "button_misc", "paddle_1", "paddle_2",
        "paddle_3", "paddle_4", "button_touchpad"
    };
    return button < SDL_arraysize(names) ? QString::fromLatin1(names[button]) : QString();
}

void SdlInputHandler::announceControllerAfterKbm(GamepadState* state)
{
    if (state == nullptr || state->controller == nullptr) return;
    uint8_t type = LI_CTYPE_UNKNOWN;
    switch (SDL_GameControllerGetType(state->controller)) {
    case SDL_CONTROLLER_TYPE_XBOX360: case SDL_CONTROLLER_TYPE_XBOXONE: type = LI_CTYPE_XBOX; break;
    case SDL_CONTROLLER_TYPE_PS3: case SDL_CONTROLLER_TYPE_PS4: case SDL_CONTROLLER_TYPE_PS5: type = LI_CTYPE_PS; break;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: type = LI_CTYPE_NINTENDO; break;
    default: break;
    }
    uint32_t buttons = 0;
    for (int i = 0; i < static_cast<int>(SDL_arraysize(k_ButtonMap)); ++i) {
        if (SDL_GameControllerHasButton(state->controller, static_cast<SDL_GameControllerButton>(i))) buttons |= k_ButtonMap[i];
    }
    uint32_t capabilities = apolloExtendedEmulationCaps(m_ControllerEmulationMode, state->controller, type);
    if (SDL_GameControllerGetBindForAxis(state->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT).bindType == SDL_CONTROLLER_BINDTYPE_AXIS ||
            SDL_GameControllerGetBindForAxis(state->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT).bindType == SDL_CONTROLLER_BINDTYPE_AXIS)
        capabilities |= LI_CCAP_ANALOG_TRIGGERS;
    if (SDL_GameControllerGetNumTouchpads(state->controller) > 0) capabilities |= LI_CCAP_TOUCHPAD;
    if (SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_ACCEL)) capabilities |= LI_CCAP_ACCEL;
    if (SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_GYRO)) capabilities |= LI_CCAP_GYRO;
    if (SDL_GameControllerHasLED(state->controller)) capabilities |= LI_CCAP_RGB_LED;
    LiSendControllerArrivalEvent(state->index, m_GamepadMask, type, buttons, capabilities);
    sendGamepadState(state);
}

void SdlInputHandler::syncControllerKbmSettings()
{
    QSettings settings;
    const bool wasEnabled = m_ControllerKbmMode;
    const bool enabled = settings.value(QStringLiteral("controllerkbmmode"), false).toBool();
    m_ControllerKbmStickSpeed = settings.value(QStringLiteral("controllerkbmstickspeed"), 100).toInt();
    m_ControllerKbmContinuous = settings.value(QStringLiteral("controllerkbmcontinuous"), true).toBool();
    m_ControllerKbmTriggerThresholds[0] = settings.value(QStringLiteral("controllerkbmlefttriggerthreshold"), 15).toInt();
    m_ControllerKbmTriggerThresholds[1] = settings.value(QStringLiteral("controllerkbmrighttriggerthreshold"), 15).toInt();
    m_ControllerKbmTriggerBehaviors[0] = settings.value(QStringLiteral("controllerkbmlefttriggerbehavior"), QStringLiteral("hold")).toString();
    m_ControllerKbmTriggerBehaviors[1] = settings.value(QStringLiteral("controllerkbmrighttriggerbehavior"), QStringLiteral("hold")).toString();
    m_ControllerKbmTriggerRepeatRates[0] = settings.value(QStringLiteral("controllerkbmlefttriggerrepeatrate"), 8).toInt();
    m_ControllerKbmTriggerRepeatRates[1] = settings.value(QStringLiteral("controllerkbmrighttriggerrepeatrate"), 8).toInt();
    m_ControllerKbmMappings.clear();
    const QVariantMap mappings = QJsonDocument::fromJson(settings.value(QStringLiteral("controllerkbmmappings"), QByteArray("{}")).toByteArray()).object().toVariantMap();
    for (auto it = mappings.constBegin(); it != mappings.constEnd(); ++it) m_ControllerKbmMappings.insert(it.key(), it.value().toString());
    const QString shortcut = settings.value(QStringLiteral("controllerkbmshortcut"), QStringLiteral("4,5")).toString();
    m_ControllerKbmShortcutMask = 0;
    for (const QString& button : shortcut.split(',', Qt::SkipEmptyParts)) {
        bool ok = false;
        const int index = button.toInt(&ok);
        if (ok && index >= 0 && index < 32) m_ControllerKbmShortcutMask |= (1u << index);
    }
    m_ControllerKbmGyroEnabled = settings.value(QStringLiteral("controllerkbmgyroenabled"), false).toBool();
    m_ControllerKbmGyroHoldMode = settings.value(QStringLiteral("controllerkbmgyroholdmode"), false).toBool();
    m_ControllerKbmGyroActivationMask = 0;
    m_ControllerKbmGyroActivationTriggers[0] = m_ControllerKbmGyroActivationTriggers[1] = false;
    const QString activationButtons = settings.value(QStringLiteral("controllerkbmgyroactivationbuttons"), QString()).toString();
    for (const QString& button : activationButtons.split(',', Qt::SkipEmptyParts)) {
        bool ok = false;
        const int index = button.toInt(&ok);
        if (!ok) continue;
        if (index >= 0 && index < 32) m_ControllerKbmGyroActivationMask |= (1u << index);
        else if (index == 100) m_ControllerKbmGyroActivationTriggers[0] = true;
        else if (index == 101) m_ControllerKbmGyroActivationTriggers[1] = true;
    }
    m_ControllerKbmGyroPrecisionEnabled = settings.value(QStringLiteral("controllerkbmgyroprecisionenabled"), false).toBool();
    m_ControllerKbmGyroPrecisionSensitivity = settings.value(QStringLiteral("controllerkbmgyroprecisionsensitivity"), 40).toInt();
    m_ControllerKbmGyroPrecisionMask = 0;
    m_ControllerKbmGyroPrecisionTriggers[0] = m_ControllerKbmGyroPrecisionTriggers[1] = false;
    const QString precisionButtons = settings.value(QStringLiteral("controllerkbmgyroprecisionbuttons"), QStringLiteral("100")).toString();
    for (const QString& button : precisionButtons.split(',', Qt::SkipEmptyParts)) {
        bool ok = false; const int index = button.toInt(&ok); if (!ok) continue;
        if (index >= 0 && index < 32) m_ControllerKbmGyroPrecisionMask |= (1u << index);
        else if (index == 100) m_ControllerKbmGyroPrecisionTriggers[0] = true;
        else if (index == 101) m_ControllerKbmGyroPrecisionTriggers[1] = true;
    }
    m_ControllerKbmGyroSensitivity = settings.value(QStringLiteral("controllerkbmgyrosensitivity"), 100).toInt();
    m_ControllerKbmGyroAxisSensitivity[0]=settings.value(QStringLiteral("controllerkbmgyroxsensitivity"),100).toInt(); m_ControllerKbmGyroAxisSensitivity[1]=settings.value(QStringLiteral("controllerkbmgyroysensitivity"),100).toInt(); m_ControllerKbmGyroAxisSensitivity[2]=settings.value(QStringLiteral("controllerkbmgyrozsensitivity"),100).toInt();
    m_ControllerKbmGyroAxisInverted[0]=settings.value(QStringLiteral("controllerkbmgyroxinverted"),false).toBool(); m_ControllerKbmGyroAxisInverted[1]=settings.value(QStringLiteral("controllerkbmgyroyinverted"),false).toBool(); m_ControllerKbmGyroAxisInverted[2]=settings.value(QStringLiteral("controllerkbmgyrozinverted"),false).toBool();
    m_GyroOverrideEnabled = settings.value(QStringLiteral("gyrooverrideenabled"), false).toBool();
    m_GyroAxisSource[0] = settings.value(QStringLiteral("gyroxsource"), 0).toInt();
    m_GyroAxisSource[1] = settings.value(QStringLiteral("gyroysource"), 1).toInt();
    m_GyroAxisSource[2] = settings.value(QStringLiteral("gyrozsource"), 2).toInt();
    m_GyroAxisInverted[0] = settings.value(QStringLiteral("gyroxinverted"), false).toBool();
    m_GyroAxisInverted[1] = settings.value(QStringLiteral("gyroyinverted"), false).toBool();
    m_GyroAxisInverted[2] = settings.value(QStringLiteral("gyrozininverted"), false).toBool();
    m_GyroAxisDisabled[0] = settings.value(QStringLiteral("gyroxdisabled"), false).toBool();
    m_GyroAxisDisabled[1] = settings.value(QStringLiteral("gyroydisabled"), false).toBool();
    m_GyroAxisDisabled[2] = settings.value(QStringLiteral("gyrozdisabled"), false).toBool();
    m_TriggerOverrideEnabled=settings.value(QStringLiteral("triggeroverrideenabled"),false).toBool(); m_TriggerOverrideThresholds[0]=settings.value(QStringLiteral("lefttriggeroverridethreshold"),50).toInt(); m_TriggerOverrideThresholds[1]=settings.value(QStringLiteral("righttriggeroverridethreshold"),50).toInt();
    m_GyroStickEnabled=settings.value(QStringLiteral("gyrostickenabled"),false).toBool(); m_GyroStickSensitivity=settings.value(QStringLiteral("gyrosticksensitivity"),100).toInt();
    m_GyroStickAxisSensitivity[0]=settings.value(QStringLiteral("gyrostickxsensitivity"),100).toInt();m_GyroStickAxisSensitivity[1]=settings.value(QStringLiteral("gyrostickysensitivity"),100).toInt();m_GyroStickAxisSensitivity[2]=settings.value(QStringLiteral("gyrostickzsensitivity"),100).toInt();
    m_GyroStickAxisInverted[0]=settings.value(QStringLiteral("gyrostickxinverted"),false).toBool();m_GyroStickAxisInverted[1]=settings.value(QStringLiteral("gyrostickyinverted"),false).toBool();m_GyroStickAxisInverted[2]=settings.value(QStringLiteral("gyrostickzinverted"),false).toBool();m_GyroStickSmoothing=settings.value(QStringLiteral("gyrosticksmoothing"),true).toBool();m_GyroStickDeadzone=settings.value(QStringLiteral("gyrostickdeadzone"),8).toInt();
    m_GyroStickShortcutMask=0; for(const QString& button:settings.value(QStringLiteral("gyrostickshortcut"),QStringLiteral("4,3")).toString().split(',',Qt::SkipEmptyParts)){bool ok=false;int index=button.toInt(&ok);if(ok&&index>=0&&index<32)m_GyroStickShortcutMask|=(1u<<index);}
    m_GyroStickHoldMode=settings.value(QStringLiteral("gyrostickholdmode"),false).toBool();m_GyroStickPrecisionEnabled=settings.value(QStringLiteral("gyrostickprecisionenabled"),false).toBool();m_GyroStickPrecisionSensitivity=settings.value(QStringLiteral("gyrostickprecisionsensitivity"),40).toInt();
    m_GyroStickActivationMask=m_GyroStickPrecisionMask=0;m_GyroStickActivationTriggers[0]=m_GyroStickActivationTriggers[1]=m_GyroStickPrecisionTriggers[0]=m_GyroStickPrecisionTriggers[1]=false;
    auto parseGyroButtons=[](const QString& value,uint32_t& mask,bool triggers[2]){for(const QString& button:value.split(',',Qt::SkipEmptyParts)){bool ok=false;int index=button.toInt(&ok);if(!ok)continue;if(index>=0&&index<32)mask|=(1u<<index);else if(index==100)triggers[0]=true;else if(index==101)triggers[1]=true;}};
    parseGyroButtons(settings.value(QStringLiteral("gyrostickactivationbuttons"),QStringLiteral("100")).toString(),m_GyroStickActivationMask,m_GyroStickActivationTriggers);parseGyroButtons(settings.value(QStringLiteral("gyrostickprecisionbuttons"),QStringLiteral("101")).toString(),m_GyroStickPrecisionMask,m_GyroStickPrecisionTriggers);
    m_ControllerKbmGyroShortcutMask = 0;
    const QString gyroShortcut = settings.value(QStringLiteral("controllerkbmgyroshortcut"), QStringLiteral("7,8")).toString();
    for (const QString& button : gyroShortcut.split(',', Qt::SkipEmptyParts)) {
        bool ok = false; const int index = button.toInt(&ok);
        if (ok && index >= 0 && index < 32) m_ControllerKbmGyroShortcutMask |= (1u << index);
    }
    if (wasEnabled == enabled) {
        for (GamepadState& state : m_GamepadState) {
            if (state.controller == nullptr) continue;
            if ((enabled || m_GyroStickEnabled) && SDL_GameControllerHasSensor(state.controller, SDL_SENSOR_GYRO)) {
                state.gyroReportPeriodMs = 4;
                SDL_GameControllerSetSensorEnabled(state.controller, SDL_SENSOR_GYRO, SDL_TRUE);
            }
            if (enabled && m_ControllerKbmContinuous && state.kbmTimer == 0)
                state.kbmTimer = SDL_AddTimer(4, controllerKbmTimerCallback, &state);
            else if ((!enabled || !m_ControllerKbmContinuous) && state.kbmTimer != 0) {
                SDL_RemoveTimer(state.kbmTimer);
                state.kbmTimer = 0;
            }
        }
        return;
    }
    if (!enabled) {
        for (GamepadState& state : m_GamepadState) {
            if (state.kbmTimer != 0) { SDL_RemoveTimer(state.kbmTimer); state.kbmTimer = 0; }
            if (state.controller != nullptr) releaseControllerKbmState(&state);
        }
        m_ControllerKbmMode = false;
        for (GamepadState& state : m_GamepadState) if (state.controller != nullptr) announceControllerAfterKbm(&state);
    }
    else {
        m_ControllerKbmMode = true;
        for (GamepadState& state : m_GamepadState) {
            if (state.controller == nullptr) continue;
            state.kbmOwner = this;
            if ((m_ControllerKbmGyroEnabled || m_GyroStickEnabled) && SDL_GameControllerHasSensor(state.controller, SDL_SENSOR_GYRO)) {
                state.gyroReportPeriodMs = 4;
                SDL_GameControllerSetSensorEnabled(state.controller, SDL_SENSOR_GYRO, SDL_TRUE);
            }
            if (m_ControllerKbmContinuous && state.kbmTimer == 0) state.kbmTimer = SDL_AddTimer(4, controllerKbmTimerCallback, &state);
        }
    }
}

void SdlInputHandler::setControllerKbmKey(int virtualKey, bool pressed)
{
    int refs = m_ControllerKbmKeyRefs.value(virtualKey);
    if (pressed) {
        if (refs++ == 0) {
            switch (virtualKey) {
            case 0x10: case 0xA0: case 0xA1: m_ControllerKbmModifiers |= MODIFIER_SHIFT; break;
            case 0x11: case 0xA2: case 0xA3: m_ControllerKbmModifiers |= MODIFIER_CTRL; break;
            case 0x12: case 0xA4: case 0xA5: m_ControllerKbmModifiers |= MODIFIER_ALT; break;
            case 0x5B: case 0x5C: m_ControllerKbmModifiers |= MODIFIER_META; break;
            }
            LiSendKeyboardEvent2(static_cast<short>(0x8000 | virtualKey), KEY_ACTION_DOWN,
                                 m_ControllerKbmModifiers, 0);
        }
        m_ControllerKbmKeyRefs.insert(virtualKey, refs);
    }
    else if (refs > 0) {
        if (--refs == 0) {
            LiSendKeyboardEvent2(static_cast<short>(0x8000 | virtualKey), KEY_ACTION_UP,
                                 m_ControllerKbmModifiers, 0);
            m_ControllerKbmKeyRefs.remove(virtualKey);
            switch (virtualKey) {
            case 0x10: case 0xA0: case 0xA1: m_ControllerKbmModifiers &= ~MODIFIER_SHIFT; break;
            case 0x11: case 0xA2: case 0xA3: m_ControllerKbmModifiers &= ~MODIFIER_CTRL; break;
            case 0x12: case 0xA4: case 0xA5: m_ControllerKbmModifiers &= ~MODIFIER_ALT; break;
            case 0x5B: case 0x5C: m_ControllerKbmModifiers &= ~MODIFIER_META; break;
            }
        }
        else m_ControllerKbmKeyRefs.insert(virtualKey, refs);
    }
}

void SdlInputHandler::setControllerKbmMouse(int button, bool pressed)
{
    int refs = m_ControllerKbmMouseRefs.value(button);
    if (pressed) {
        if (refs++ == 0) LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, button);
        m_ControllerKbmMouseRefs.insert(button, refs);
    }
    else if (refs > 0) {
        if (--refs == 0) {
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
            m_ControllerKbmMouseRefs.remove(button);
        }
        else m_ControllerKbmMouseRefs.insert(button, refs);
    }
}

void SdlInputHandler::updateControllerKbmGyroHoldState(GamepadState* state)
{
    if (!m_ControllerKbmGyroHoldMode) {
        state->gyroHoldActive = false;
        return;
    }
    const bool buttonActive = (state->chord.down & m_ControllerKbmGyroActivationMask) != 0;
    const bool leftTriggerActive = m_ControllerKbmGyroActivationTriggers[0] && state->lt >= 15 * 255 / 100;
    const bool rightTriggerActive = m_ControllerKbmGyroActivationTriggers[1] && state->rt >= 15 * 255 / 100;
    const bool active = buttonActive || leftTriggerActive || rightTriggerActive;
    if (active != state->gyroHoldActive) {
        state->gyroHoldActive = active;
        m_ControllerKbmLastGyroTime = 0;
        Session::get()->notifyControllerKbmGyro(active);
    }
}

void SdlInputHandler::handleControllerKbmAction(GamepadState*, const QString& source, bool pressed)
{
    const QString action = m_ControllerKbmMappings.value(source);
    if (action.startsWith(QStringLiteral("key:"))) {
        bool ok = false; const int key = action.mid(4).toInt(&ok);
        if (ok) setControllerKbmKey(key, pressed);
    }
    else if (action == QStringLiteral("mouse_left")) setControllerKbmMouse(BUTTON_LEFT, pressed);
    else if (action == QStringLiteral("mouse_right")) setControllerKbmMouse(BUTTON_RIGHT, pressed);
    else if (action == QStringLiteral("mouse_middle")) setControllerKbmMouse(BUTTON_MIDDLE, pressed);
    else if (action == QStringLiteral("mouse_back")) setControllerKbmMouse(BUTTON_X1, pressed);
    else if (action == QStringLiteral("mouse_forward")) setControllerKbmMouse(BUTTON_X2, pressed);
    else if (pressed && action == QStringLiteral("wheel_up")) LiSendHighResScrollEvent(120);
    else if (pressed && action == QStringLiteral("wheel_down")) LiSendHighResScrollEvent(-120);
    else if (pressed && action.startsWith(QStringLiteral("directed_flick:"))) {
        const QStringList parts = action.split(':');
        if (parts.size() == 3) {
            const int distance = qBound(50, parts[2].toInt(), 4000);
            int x = 0, y = 0;
            if (parts[1] == QStringLiteral("left")) x = -distance;
            else if (parts[1] == QStringLiteral("right")) x = distance;
            else if (parts[1] == QStringLiteral("up")) y = -distance;
            else if (parts[1] == QStringLiteral("down")) y = distance;
            for (int i = 0; i < 4; ++i) LiSendMouseMoveEvent(x / 4, y / 4);
        }
    }
}

void SdlInputHandler::updateGyroRightStick(GamepadState* state,const float data[3])
{
    const bool activationHeld=(state->chord.down&m_GyroStickActivationMask)!=0||(m_GyroStickActivationTriggers[0]&&state->lt>24)||(m_GyroStickActivationTriggers[1]&&state->rt>24);
    if(m_ControllerKbmMode||!m_GyroStickEnabled||(m_GyroStickHoldMode&&!activationHeld)){const bool hadOutput=state->gyroStickX!=0||state->gyroStickY!=0;state->gyroStickX=state->gyroStickY=0;state->gyroStickFilteredX=state->gyroStickFilteredY=0;if(hadOutput)sendGamepadState(state);return;}
    const bool precisionHeld=m_GyroStickPrecisionEnabled&&((state->chord.down&m_GyroStickPrecisionMask)!=0||(m_GyroStickPrecisionTriggers[0]&&state->lt>24)||(m_GyroStickPrecisionTriggers[1]&&state->rt>24));
    const int activeSensitivity=precisionHeld?m_GyroStickPrecisionSensitivity:m_GyroStickSensitivity;
    float axis[3]={data[0],data[1],data[2]};
    for(int i=0;i<3;i++){if(m_GyroStickAxisInverted[i])axis[i]=-axis[i];axis[i]*=(activeSensitivity/100.0f)*(m_GyroStickAxisSensitivity[i]/100.0f);}
    float x=(axis[1]+axis[2])*0.34f,y=axis[0]*0.34f;
    const float mag=qSqrt(x*x+y*y),dead=m_GyroStickDeadzone/100.0f;
    if(mag<=dead)x=y=0;else if(mag>0){const float curved=qPow(qMin(1.0f,(mag-dead)/qMax(0.01f,1.0f-dead)),0.72f);x*=curved/mag;y*=curved/mag;}
    const float alpha=m_GyroStickSmoothing?0.38f:1.0f;state->gyroStickFilteredX+=(x-state->gyroStickFilteredX)*alpha;state->gyroStickFilteredY+=(y-state->gyroStickFilteredY)*alpha;
    state->gyroStickX=static_cast<short>(qBound(-1.0f,state->gyroStickFilteredX,1.0f)*32767);state->gyroStickY=static_cast<short>(qBound(-1.0f,state->gyroStickFilteredY,1.0f)*32767);
    sendGamepadState(state);
}

ChordMasks SdlInputHandler::currentChordMasks() const
{
    ChordMasks masks;
    masks.quickMenu = m_ControllerKbmShortcutMask;
    masks.gyroToggle = m_ControllerKbmGyroShortcutMask;
    masks.gyroStick = m_GyroStickShortcutMask;
    masks.kbmMode = m_ControllerKbmMode;
    masks.gyroHoldMode = m_ControllerKbmGyroHoldMode;
    return masks;
}

void SdlInputHandler::applyChordRelease(GamepadState* state, uint32_t sdlButtonBits)
{
    if (state == nullptr || sdlButtonBits == 0) {
        return;
    }
    for (unsigned i = 0; i < 32 && i < SDL_arraysize(k_ButtonMap); ++i) {
        if ((sdlButtonBits & (1u << i)) == 0) {
            continue;
        }
        if (m_ControllerKbmMode) {
            handleControllerKbmAction(state, controllerKbmSourceForButton(i), false);
        }
        else {
            state->buttons &= ~k_ButtonMap[i];
        }
    }
    if (!m_ControllerKbmMode && state->mouseEmulationTimer == 0) {
        sendGamepadState(state);
    }
}

void SdlInputHandler::resetControllerChord(GamepadState* state)
{
    if (state == nullptr) {
        return;
    }
    applyChordRelease(state, chordTakeForwarded(state->chord));
}

void SdlInputHandler::resetAllControllerChords()
{
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (m_GamepadState[i].controller != nullptr) {
            resetControllerChord(&m_GamepadState[i]);
        }
    }
}

void SdlInputHandler::bindSelectedDualSenseAudio(GamepadState* state, bool bluetooth, const char* path)
{
    if (state == nullptr) {
        return;
    }
    if (m_SelectedDualSenseAudioIndex >= 0 && m_SelectedDualSenseAudioIndex != state->index) {
        return;
    }
    DualSenseAudioRenderer::instance().setController(state->controller, bluetooth, path, state->index);
    m_SelectedDualSenseAudioIndex = state->index;
}

void SdlInputHandler::unbindSelectedDualSenseAudio(GamepadState* state)
{
    if (state == nullptr || m_SelectedDualSenseAudioIndex != state->index) {
        return;
    }
    DualSenseAudioRenderer::instance().setController(nullptr, false, nullptr, -1);
    m_SelectedDualSenseAudioIndex = -1;
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        GamepadState* other = &m_GamepadState[i];
        if (other == state || other->controller == nullptr) {
            continue;
        }
        if (SDL_GameControllerGetType(other->controller) == SDL_CONTROLLER_TYPE_PS5) {
            const char* path = SDL_JoystickPath(SDL_GameControllerGetJoystick(other->controller));
            bindSelectedDualSenseAudio(other, other->dualSenseBluetooth, path);
            break;
        }
    }
}

void SdlInputHandler::updateControllerKbmStick(GamepadState* state, const QString& source,
                                                float x, float y, int slot)
{
    const QString action = m_ControllerKbmMappings.value(source);
    if (action != QStringLiteral("basic_wasd") && action != QStringLiteral("basic_arrows")) return;
    const int keys[2][4] = {{0x41, 0x44, 0x57, 0x53}, {0x25, 0x27, 0x26, 0x28}};
    const int row = action == QStringLiteral("basic_wasd") ? 0 : 1;
    const bool targets[4] = {x < -0.45f, x > 0.45f, y < -0.45f, y > 0.45f};
    const float releases[4] = {x, -x, y, -y};
    for (int d = 0; d < 4; ++d) {
        const int index = slot * 8 + row * 4 + d;
        bool next = state->kbmDirectional[index];
        if (!next && targets[d]) next = true;
        else if (next && releases[d] > -0.30f) next = false;
        if (next != state->kbmDirectional[index]) {
            state->kbmDirectional[index] = next;
            setControllerKbmKey(keys[row][d], next);
        }
    }
}

void SdlInputHandler::pollControllerKbm(GamepadState* state)
{
    const float axes[2][2] = {
        {kbmDeadzone(state->lsX / 32767.0f), kbmDeadzone(-state->lsY / 32767.0f)},
        {kbmDeadzone(state->rsX / 32767.0f), kbmDeadzone(-state->rsY / 32767.0f)}
    };
    const QString sources[2] = {QStringLiteral("left_stick"), QStringLiteral("right_stick")};
    for (int stick = 0; stick < 2; ++stick) {
        const QString action = m_ControllerKbmMappings.value(sources[stick]);
        updateControllerKbmStick(state, sources[stick], axes[stick][0], axes[stick][1], stick);
        if (action == QStringLiteral("mouse_move")) {
            const float scale = (m_ControllerKbmStickSpeed / 100.0f) * 18.0f * (m_ControllerKbmContinuous ? 0.25f : 1.0f);
            state->kbmMouseRemainderX += std::copysign(axes[stick][0] * axes[stick][0], axes[stick][0]) * scale;
            state->kbmMouseRemainderY += std::copysign(axes[stick][1] * axes[stick][1], axes[stick][1]) * scale;
            const int dx = static_cast<int>(state->kbmMouseRemainderX);
            const int dy = static_cast<int>(state->kbmMouseRemainderY);
            state->kbmMouseRemainderX -= dx; state->kbmMouseRemainderY -= dy;
            if (dx || dy) LiSendMouseMoveEvent(dx, dy);
        }
        else if (action == QStringLiteral("scroll")) {
            state->kbmScrollRemainderX += std::copysign(axes[stick][0] * axes[stick][0], axes[stick][0]) * 60.0f;
            state->kbmScrollRemainderY += -std::copysign(axes[stick][1] * axes[stick][1], axes[stick][1]) * 60.0f;
            const int dx = static_cast<int>(state->kbmScrollRemainderX);
            const int dy = static_cast<int>(state->kbmScrollRemainderY);
            state->kbmScrollRemainderX -= dx; state->kbmScrollRemainderY -= dy;
            if (dx) LiSendHighResHScrollEvent(dx);
            if (dy) LiSendHighResScrollEvent(dy);
        }
    }
    if (m_ControllerKbmTriggerBehaviors[0] == QStringLiteral("repeat") ||
            m_ControllerKbmTriggerBehaviors[1] == QStringLiteral("repeat")) {
        const uint32_t now = SDL_GetTicks();
        for (int trigger = 0; trigger < 2; ++trigger) {
            const uint32_t period = 1000 / qMax(1, m_ControllerKbmTriggerRepeatRates[trigger]);
            if (m_ControllerKbmTriggerBehaviors[trigger] == QStringLiteral("repeat") &&
                    state->kbmTriggerDown[trigger] && SDL_TICKS_PASSED(now, state->kbmTriggerRepeatAt[trigger])) {
                const QString source = trigger == 0 ? QStringLiteral("trigger_left") : QStringLiteral("trigger_right");
                handleControllerKbmAction(state, source, true);
                handleControllerKbmAction(state, source, false);
                state->kbmTriggerRepeatAt[trigger] = now + period;
            }
        }
    }
}

Uint32 SdlInputHandler::controllerKbmTimerCallback(Uint32 interval, void* param)
{
    GamepadState* state = static_cast<GamepadState*>(param);
    // SDL timer callbacks run on a worker thread. Never touch Qt containers or
    // the mutable controller state here. Queue a synthetic axis event so the
    // regular SDL input thread performs the poll in-order with real input.
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_CONTROLLERAXISMOTION;
    event.caxis.timestamp = SDL_GetTicks();
    event.caxis.which = state->jsId;
    event.caxis.axis = SDL_CONTROLLER_AXIS_MAX;
    SDL_PushEvent(&event);
    return interval;
}

void SdlInputHandler::releaseControllerKbmState(GamepadState* state)
{
    if (!m_ControllerKbmMode) return;
    for (auto it = m_ControllerKbmKeyRefs.constBegin(); it != m_ControllerKbmKeyRefs.constEnd(); ++it)
        LiSendKeyboardEvent2(static_cast<short>(0x8000 | it.key()), KEY_ACTION_UP, 0, 0);
    for (auto it = m_ControllerKbmMouseRefs.constBegin(); it != m_ControllerKbmMouseRefs.constEnd(); ++it)
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, it.key());
    m_ControllerKbmKeyRefs.clear(); m_ControllerKbmMouseRefs.clear(); m_ControllerKbmModifiers = 0;
    SDL_memset(state->kbmDirectional, 0, sizeof(state->kbmDirectional));
}

void SdlInputHandler::pollDualSenseHeadsets()
{
    if (m_DualSenseAudioMode != StreamingPreferences::DSAM_AUTO) {
        return;
    }

    const uint32_t now = SDL_GetTicks();
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        GamepadState* state = &m_GamepadState[i];
        if (state->controller == nullptr ||
                SDL_GameControllerGetType(state->controller) != SDL_CONTROLLER_TYPE_PS5 ||
                (state->dualSenseHeadsetStateKnown &&
                 !SDL_TICKS_PASSED(now, state->lastDualSenseHeadsetPollTime + 100))) {
            continue;
        }

        state->lastDualSenseHeadsetPollTime = now;
        const bool connected = queryDualSenseHeadset(state->controller);
        if (!state->dualSenseHeadsetStateKnown || connected != state->dualSenseHeadsetConnected) {
            state->dualSenseHeadsetStateKnown = true;
            state->dualSenseHeadsetConnected = connected;
            if (m_SelectedDualSenseAudioIndex == state->index) {
                if (state->dualSenseBluetooth) {
                    DualSenseAudioRenderer::instance().setBluetoothHeadsetActive(connected);
                }
                else {
                    activateDualSenseAudio(state->controller,
                        connected ? StreamingPreferences::DSAM_USB_HEADSET : StreamingPreferences::DSAM_AUTO);
                    SdlAudioRenderer::setDualSenseHeadsetActive(connected);
                }
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "DualSense %s headset %s; automatic main audio mirror %s",
                        state->dualSenseBluetooth ? "Bluetooth" : "wired",
                        connected ? "connected" : "disconnected",
                        connected ? "enabled" : "disabled");
            publishControllerStatus(state);
        }
    }
}

void SdlInputHandler::sendGamepadBatteryState(GamepadState* state, SDL_JoystickPowerLevel level)
{
    uint8_t batteryPercentage;
    uint8_t batteryState;

    // SDL's battery reporting capabilities are quite limited. Notably, we cannot
    // tell the battery level while charging (or even if a battery is present).
    // We also cannot tell the percentage of charge exactly in any case.
    switch (level)
    {
    case SDL_JOYSTICK_POWER_UNKNOWN:
        batteryState = LI_BATTERY_STATE_UNKNOWN;
        batteryPercentage = LI_BATTERY_PERCENTAGE_UNKNOWN;
        break;
    case SDL_JOYSTICK_POWER_WIRED:
        batteryState = LI_BATTERY_STATE_CHARGING;
        batteryPercentage = LI_BATTERY_PERCENTAGE_UNKNOWN;
        break;
    case SDL_JOYSTICK_POWER_EMPTY:
        batteryState = LI_BATTERY_STATE_DISCHARGING;
        batteryPercentage = 5;
        break;
    case SDL_JOYSTICK_POWER_LOW:
        batteryState = LI_BATTERY_STATE_DISCHARGING;
        batteryPercentage = 20;
        break;
    case SDL_JOYSTICK_POWER_MEDIUM:
        batteryState = LI_BATTERY_STATE_DISCHARGING;
        batteryPercentage = 50;
        break;
    case SDL_JOYSTICK_POWER_FULL:
        batteryState = LI_BATTERY_STATE_DISCHARGING;
        batteryPercentage = 90;
        break;
    default:
        return;
    }

    LiSendControllerBatteryEvent(state->index, batteryState, batteryPercentage);
    state->batteryState = batteryState;
    state->batteryPercentage = batteryPercentage == LI_BATTERY_PERCENTAGE_UNKNOWN ? -1 : batteryPercentage;
    publishControllerStatus(state);

    QSettings settings;
    const bool warningEnabled = settings.value(QStringLiteral("controllerBatteryWarningEnabled"), true).toBool();
    const int warningThreshold = settings.value(QStringLiteral("controllerBatteryWarningThreshold"), 10).toInt();
    if (batteryState == LI_BATTERY_STATE_DISCHARGING && state->batteryPercentage >= 0) {
        if (warningEnabled && state->batteryPercentage <= warningThreshold && !state->batteryWarningShown) {
            state->batteryWarningShown = true;
            Session::get()->notifyControllerBatteryLow(state->batteryPercentage);
        }
        else if (state->batteryPercentage > warningThreshold) state->batteryWarningShown = false;
    }
    else state->batteryWarningShown = false;
}

Uint32 SdlInputHandler::mouseEmulationTimerCallback(Uint32 interval, void *param)
{
    auto gamepad = reinterpret_cast<GamepadState*>(param);

    int rawX;
    int rawY;

    // Determine which analog stick is currently receiving the strongest input
    if (abs(gamepad->lsX) + abs(gamepad->lsY) > abs(gamepad->rsX) + abs(gamepad->rsY)) {
        rawX = gamepad->lsX;
        rawY = -gamepad->lsY;
    }
    else {
        rawX = gamepad->rsX;
        rawY = -gamepad->rsY;
    }

    float deltaX;
    float deltaY;

    // Produce a base vector for mouse movement with increased speed as we deviate further from center
    deltaX = qPow(rawX / 32766.0f * MOUSE_EMULATION_MOTION_MULTIPLIER, 3);
    deltaY = qPow(rawY / 32766.0f * MOUSE_EMULATION_MOTION_MULTIPLIER, 3);

    // Enforce deadzones
    deltaX = qAbs(deltaX) > MOUSE_EMULATION_DEADZONE ? deltaX - MOUSE_EMULATION_DEADZONE : 0;
    deltaY = qAbs(deltaY) > MOUSE_EMULATION_DEADZONE ? deltaY - MOUSE_EMULATION_DEADZONE : 0;

    if (deltaX != 0 || deltaY != 0) {
        LiSendMouseMoveEvent((short)deltaX, (short)deltaY);
    }

    return interval;
}

void SdlInputHandler::handleControllerAxisEvent(SDL_ControllerAxisEvent* event)
{
    SDL_JoystickID gameControllerId = event->which;
    GamepadState* state = findStateForGamepad(gameControllerId);
    if (state == NULL) {
        return;
    }

    syncControllerKbmSettings();
    if (m_ControllerKbmMode) {
        switch (event->axis) {
        case SDL_CONTROLLER_AXIS_LEFTX: state->lsX = event->value; break;
        case SDL_CONTROLLER_AXIS_LEFTY: state->lsY = -qMax(event->value, static_cast<short>(-32767)); break;
        case SDL_CONTROLLER_AXIS_RIGHTX: state->rsX = event->value; break;
        case SDL_CONTROLLER_AXIS_RIGHTY: state->rsY = -qMax(event->value, static_cast<short>(-32767)); break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: state->lt = static_cast<unsigned char>(event->value * 255UL / 32767); break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: state->rt = static_cast<unsigned char>(event->value * 255UL / 32767); break;
        default: break;
        }
        if (m_ControllerKbmGyroHoldMode)
            updateControllerKbmGyroHoldState(state);
        const unsigned char triggers[2] = {state->lt, state->rt};
        for (int trigger = 0; trigger < 2; ++trigger) {
            const bool down = triggers[trigger] >= (m_ControllerKbmTriggerThresholds[trigger] * 255 / 100);
            if (down != state->kbmTriggerDown[trigger]) {
                state->kbmTriggerDown[trigger] = down;
                const QString source = trigger == 0 ? QStringLiteral("trigger_left") : QStringLiteral("trigger_right");
                if (m_ControllerKbmTriggerBehaviors[trigger] == QStringLiteral("hold"))
                    handleControllerKbmAction(state, source, down);
                else if (down) {
                    handleControllerKbmAction(state, source, true);
                    handleControllerKbmAction(state, source, false);
                    state->kbmTriggerRepeatAt[trigger] = SDL_GetTicks() + 1000 / qMax(1, m_ControllerKbmTriggerRepeatRates[trigger]);
                }
            }
        }
        if (!m_ControllerKbmContinuous || event->axis == SDL_CONTROLLER_AXIS_MAX)
            pollControllerKbm(state);
        return;
    }

    // Batch all pending axis motion events for this gamepad to save CPU time
    SDL_Event nextEvent;
    for (;;) {
        switch (event->axis)
        {
            case SDL_CONTROLLER_AXIS_LEFTX:
                state->lsX = event->value;
                break;
            case SDL_CONTROLLER_AXIS_LEFTY:
                // Signed values have one more negative value than
                // positive value, so inverting the sign on -32768
                // could actually cause the value to overflow and
                // wrap around to be negative again. Avoid that by
                // capping the value at 32767.
                state->lsY = -qMax(event->value, (short)-32767);
                break;
            case SDL_CONTROLLER_AXIS_RIGHTX:
                state->rsX = event->value;
                break;
            case SDL_CONTROLLER_AXIS_RIGHTY:
                state->rsY = -qMax(event->value, (short)-32767);
                break;
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                state->lt = (unsigned char)(event->value * 255UL / 32767);
                break;
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                state->rt = (unsigned char)(event->value * 255UL / 32767);
                break;
            default:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Unhandled controller axis: %d",
                            event->axis);
                return;
        }

        // Check for another event to batch with
        if (SDL_PeepEvents(&nextEvent, 1, SDL_PEEKEVENT, SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERAXISMOTION) <= 0) {
            break;
        }

        event = &nextEvent.caxis;
        if (event->which != gameControllerId) {
            // Stop batching if a different gamepad interrupts us
            break;
        }

        // Remove the next event to batch
        SDL_PeepEvents(&nextEvent, 1, SDL_GETEVENT, SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERAXISMOTION);
    }

    if (m_ControllerKbmMode && m_ControllerKbmGyroHoldMode)
        updateControllerKbmGyroHoldState(state);

    // Only send the gamepad state to the host if it's not in mouse emulation mode
    if (state->mouseEmulationTimer == 0) {
        if (m_TriggerOverrideEnabled) {
            if (state->lt >= m_TriggerOverrideThresholds[0] * 255 / 100) state->lt = 255;
            if (state->rt >= m_TriggerOverrideThresholds[1] * 255 / 100) state->rt = 255;
        }
        sendGamepadState(state);
    }
}

void SdlInputHandler::handleControllerButtonEvent(SDL_ControllerButtonEvent* event)
{
    syncControllerKbmSettings();
    if (event->button >= SDL_arraysize(k_ButtonMap)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "No mapping for gamepad button: %u",
                    event->button);
        return;
    }

    GamepadState* state = findStateForGamepad(event->which);
    if (state == NULL) {
        return;
    }

    if (m_SwapFaceButtons) {
        switch (event->button) {
        case SDL_CONTROLLER_BUTTON_A:
            event->button = SDL_CONTROLLER_BUTTON_B;
            break;
        case SDL_CONTROLLER_BUTTON_B:
            event->button = SDL_CONTROLLER_BUTTON_A;
            break;
        case SDL_CONTROLLER_BUTTON_X:
            event->button = SDL_CONTROLLER_BUTTON_Y;
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            event->button = SDL_CONTROLLER_BUTTON_X;
            break;
        }
    }

    // The DualSense mute button always controls the global forwarded microphone,
    // including while Controller KBM mode reserves all other controller inputs.
    if (event->button == SDL_CONTROLLER_BUTTON_MISC1 &&
            SDL_GameControllerGetType(state->controller) == SDL_CONTROLLER_TYPE_PS5) {
        if (event->state == SDL_PRESSED) {
            const bool muted = !MicCapture::isMuted();
            MicCapture::setMuted(muted);
            if (state->dualSenseBluetooth) {
                if (m_SelectedDualSenseAudioIndex == state->index) {
                    DualSenseAudioRenderer::instance().setBluetoothMicLed(muted);
                }
            }
            else {
                DualSenseOutputReport micLed = {};
                micLed.validFlag1 = 0x01;
                micLed.muteButtonLed = muted ? 0x01 : 0x00;
                SDL_GameControllerSendEffect(state->controller, &micLed, sizeof(micLed));
            }
            Session::get()->notifyMicrophoneMute(muted);
        }
        return;
    }

    const bool pressed = event->state == SDL_PRESSED;
    const ChordResult chord = chordOnButton(state->chord, event->button, pressed, currentChordMasks());
    if (m_ControllerKbmMode && m_ControllerKbmGyroHoldMode)
        updateControllerKbmGyroHoldState(state);

    if (chord.action == ChordAction::Suppress) {
        return;
    }
    if (chord.action == ChordAction::FireQuickMenu) {
        applyChordRelease(state, chord.releaseBits);
        releaseControllerKbmState(state);
        setCaptureActive(false);
        QMetaObject::invokeMethod(StreamingPreferences::get(),
                                  "requestControllerKbmConfiguration",
                                  Qt::QueuedConnection);
        return;
    }
    if (chord.action == ChordAction::FireGyroToggle) {
        applyChordRelease(state, chord.releaseBits);
        m_ControllerKbmGyroEnabled = !m_ControllerKbmGyroEnabled;
        QSettings().setValue(QStringLiteral("controllerkbmgyroenabled"), m_ControllerKbmGyroEnabled);
        m_ControllerKbmLastGyroTime = 0;
        Session::get()->notifyControllerKbmGyro(m_ControllerKbmGyroEnabled);
        return;
    }
    if (chord.action == ChordAction::FireGyroStick) {
        applyChordRelease(state, chord.releaseBits);
        m_GyroStickEnabled = !m_GyroStickEnabled;
        QSettings().setValue(QStringLiteral("gyrostickenabled"), m_GyroStickEnabled);
        Session::get()->notifyGyroStick(m_GyroStickEnabled);
        if (!m_GyroStickEnabled) {
            state->gyroStickX = state->gyroStickY = 0;
            state->gyroStickFilteredX = state->gyroStickFilteredY = 0;
            sendGamepadState(state);
        }
        return;
    }

    if (m_ControllerKbmMode) {
        handleControllerKbmAction(state, controllerKbmSourceForButton(event->button), pressed);
        return;
    }

    if (event->state == SDL_PRESSED) {
        state->buttons |= k_ButtonMap[event->button];

        if (event->button == SDL_CONTROLLER_BUTTON_START) {
            state->lastStartDownTime = SDL_GetTicks();
        }
        else if (state->mouseEmulationTimer != 0) {
            if (event->button == SDL_CONTROLLER_BUTTON_A) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_B) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_X) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_MIDDLE);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_X1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_X2);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                LiSendScrollEvent(1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                LiSendScrollEvent(-1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                LiSendHScrollEvent(1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
                LiSendHScrollEvent(-1);
            }
        }
    }
    else {
        state->buttons &= ~k_ButtonMap[event->button];

        if (event->button == SDL_CONTROLLER_BUTTON_START) {
            if (SDL_GetTicks() - state->lastStartDownTime > MOUSE_EMULATION_LONG_PRESS_TIME) {
                if (state->mouseEmulationTimer != 0) {
                    SDL_RemoveTimer(state->mouseEmulationTimer);
                    state->mouseEmulationTimer = 0;

                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Mouse emulation deactivated");
                    Session::get()->notifyMouseEmulationMode(false);
                }
                else if (m_GamepadMouse) {
                    // Send the start button up event to the host, since we won't do it below
                    sendGamepadState(state);

                    state->mouseEmulationTimer = SDL_AddTimer(MOUSE_EMULATION_POLLING_INTERVAL, SdlInputHandler::mouseEmulationTimerCallback, state);

                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Mouse emulation active");
                    Session::get()->notifyMouseEmulationMode(true);
                }
            }
        }
        else if (state->mouseEmulationTimer != 0) {
            if (event->button == SDL_CONTROLLER_BUTTON_A) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_B) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_X) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MIDDLE);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_X1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_X2);
            }
        }
    }

    // Handle Start+Select+L1+R1 as a gamepad quit combo
    if (state->buttons == (PLAY_FLAG | BACK_FLAG | LB_FLAG | RB_FLAG) && qgetenv("NO_GAMEPAD_QUIT") != "1") {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected quit gamepad button combo");

        // Push a quit event to the main loop
        SDL_Event event;
        event.type = SDL_QUIT;
        event.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&event);

        // Clear buttons down on this gamepad
        LiSendMultiControllerEvent(state->index, m_GamepadMask,
                                   0, 0, 0, 0, 0, 0, 0);
        return;
    }

    // Handle Select+L1+R1+X as a gamepad overlay combo
    if (state->buttons == (BACK_FLAG | LB_FLAG | RB_FLAG | X_FLAG)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected stats toggle gamepad combo");

        // Toggle the stats overlay
        Session::get()->getOverlayManager().setOverlayState(Overlay::OverlayDebug,
                                                            !Session::get()->getOverlayManager().isOverlayEnabled(Overlay::OverlayDebug));

        // Clear buttons down on this gamepad
        LiSendMultiControllerEvent(state->index, m_GamepadMask,
                                   0, 0, 0, 0, 0, 0, 0);
        return;
    }

    // Only send the gamepad state to the host if it's not in mouse emulation mode
    if (state->mouseEmulationTimer == 0) {
        sendGamepadState(state);
    }
}

#if SDL_VERSION_ATLEAST(2, 0, 14)

void SdlInputHandler::handleControllerSensorEvent(SDL_ControllerSensorEvent* event)
{
    // Sensor events can continue without any accompanying button/axis event.
    // Refresh here so Quick Menu gyro changes also take effect in a stationary stream.
    static uint32_t lastSettingsSync = 0;
    if (SDL_TICKS_PASSED(event->timestamp, lastSettingsSync + 100)) {
        syncControllerKbmSettings();
        lastSettingsSync = event->timestamp;
    }
    GamepadState* state = findStateForGamepad(event->which);
    if (state == NULL) {
        return;
    }

    switch (event->sensor) {
    case SDL_SENSOR_ACCEL:
        if (state->accelReportPeriodMs &&
                SDL_TICKS_PASSED(event->timestamp, state->lastAccelEventTime + state->accelReportPeriodMs) &&
                memcmp(event->data, state->lastAccelEventData, sizeof(event->data)) != 0) {
            memcpy(state->lastAccelEventData, event->data, sizeof(event->data));
            state->lastAccelEventTime = event->timestamp;

            if (m_ControllerKbmMode) LiSendControllerMotionEvent((uint8_t)state->index, LI_MOTION_TYPE_ACCEL, 0, 0, 0);
            else LiSendControllerMotionEvent((uint8_t)state->index, LI_MOTION_TYPE_ACCEL, event->data[0], event->data[1], event->data[2]);
        }
        break;
    case SDL_SENSOR_GYRO:
        updateGyroRightStick(state, event->data);
        if (m_ControllerKbmMode && m_ControllerKbmGyroEnabled &&
                (!m_ControllerKbmGyroHoldMode || state->gyroHoldActive)) {
            const uint32_t now = event->timestamp;
            if (m_ControllerKbmLastGyroTime != 0 && now - m_ControllerKbmLastGyroTime < 100) {
                const float dt = (now - m_ControllerKbmLastGyroTime) / 1000.0f;
                float axis[3]={event->data[0],event->data[1],event->data[2]};
                for(int i=0;i<3;i++){if(m_ControllerKbmGyroAxisInverted[i])axis[i]=-axis[i];axis[i]*=m_ControllerKbmGyroAxisSensitivity[i]/100.0f;}
                const float yaw = std::abs(axis[1]) >= std::abs(axis[2]) ? axis[1] : axis[2];
                const bool precisionActive = m_ControllerKbmGyroPrecisionEnabled &&
                    (((state->chord.down & m_ControllerKbmGyroPrecisionMask) != 0) ||
                     (m_ControllerKbmGyroPrecisionTriggers[0] && state->lt >= 15 * 255 / 100) ||
                     (m_ControllerKbmGyroPrecisionTriggers[1] && state->rt >= 15 * 255 / 100));
                const float precisionScale = precisionActive ? m_ControllerKbmGyroPrecisionSensitivity / 100.0f : 1.0f;
                const float scale = 57.2957795f * dt * (m_ControllerKbmGyroSensitivity / 100.0f) * precisionScale;
                m_ControllerKbmGyroRemainderX += yaw * scale;
                m_ControllerKbmGyroRemainderY += axis[0] * scale;
                const int dx = static_cast<int>(m_ControllerKbmGyroRemainderX);
                const int dy = static_cast<int>(m_ControllerKbmGyroRemainderY);
                m_ControllerKbmGyroRemainderX -= dx; m_ControllerKbmGyroRemainderY -= dy;
                if (dx || dy) LiSendMouseMoveEvent(dx, dy);
            }
            m_ControllerKbmLastGyroTime = now;
            LiSendControllerMotionEvent((uint8_t)state->index, LI_MOTION_TYPE_GYRO, 0, 0, 0);
            return;
        }
        if (state->gyroReportPeriodMs &&
                SDL_TICKS_PASSED(event->timestamp, state->lastGyroEventTime + state->gyroReportPeriodMs) &&
                memcmp(event->data, state->lastGyroEventData, sizeof(event->data)) != 0) {
            memcpy(state->lastGyroEventData, event->data, sizeof(event->data));
            state->lastGyroEventTime = event->timestamp;

            float gyro[3] = { event->data[0], event->data[1], event->data[2] };
            // This menu can be changed while streaming, so use the live
            // preferences instead of the snapshot captured at stream startup.
            const auto* prefs = StreamingPreferences::get();
            const bool gyroOverrideEnabled = prefs->gyroOverrideEnabled;
            const int gyroAxisSource[3] = {
                prefs->gyroXAxisSource, prefs->gyroYAxisSource, prefs->gyroZAxisSource
            };
            const bool gyroAxisInverted[3] = {
                prefs->gyroXAxisInverted, prefs->gyroYAxisInverted, prefs->gyroZAxisInverted
            };
            const bool gyroAxisDisabled[3] = {
                prefs->gyroXAxisDisabled, prefs->gyroYAxisDisabled, prefs->gyroZAxisDisabled
            };
            if (gyroOverrideEnabled) {
                const float rawGyro[3] = { event->data[0], event->data[1], event->data[2] };
                for (int axis = 0; axis < 3; axis++) {
                    if (gyroAxisDisabled[axis]) {
                        gyro[axis] = 0.0f;
                    }
                    else {
                        gyro[axis] = rawGyro[gyroAxisSource[axis]];
                        if (gyroAxisInverted[axis]) {
                            gyro[axis] = -gyro[axis];
                        }
                    }
                }
            }

            // Convert rad/s to deg/s after applying the optional axis override.
            if (m_ControllerKbmMode) LiSendControllerMotionEvent((uint8_t)state->index, LI_MOTION_TYPE_GYRO, 0, 0, 0);
            else LiSendControllerMotionEvent((uint8_t)state->index, LI_MOTION_TYPE_GYRO,
                                             gyro[0] * 57.2957795f,
                                             gyro[1] * 57.2957795f,
                                             gyro[2] * 57.2957795f);
        }
        break;
    }
}

void SdlInputHandler::handleControllerTouchpadEvent(SDL_ControllerTouchpadEvent* event)
{
    GamepadState* state = findStateForGamepad(event->which);
    if (state == NULL) {
        return;
    }

    uint8_t eventType;
    switch (event->type) {
    case SDL_CONTROLLERTOUCHPADDOWN:
        eventType = LI_TOUCH_EVENT_DOWN;
        break;
    case SDL_CONTROLLERTOUCHPADUP:
        eventType = LI_TOUCH_EVENT_UP;
        break;
    case SDL_CONTROLLERTOUCHPADMOTION:
        eventType = LI_TOUCH_EVENT_MOVE;
        break;
    default:
        return;
    }

    LiSendControllerTouchEvent2((uint8_t)state->index, eventType,
                                (uint8_t)event->touchpad, event->finger,
                                event->x, event->y, event->pressure);
}

#endif

#if SDL_VERSION_ATLEAST(2, 24, 0)

void SdlInputHandler::handleJoystickBatteryEvent(SDL_JoyBatteryEvent* event)
{
    GamepadState* state = findStateForGamepad(event->which);
    if (state == NULL) {
        return;
    }

    sendGamepadBatteryState(state, event->level);
}

#endif

uint8_t SdlInputHandler::moonlightControllerType(SDL_GameController* controller)
{
    switch (SDL_GameControllerGetType(controller)) {
    case SDL_CONTROLLER_TYPE_XBOX360:
    case SDL_CONTROLLER_TYPE_XBOXONE:
        return LI_CTYPE_XBOX;
    case SDL_CONTROLLER_TYPE_PS3:
    case SDL_CONTROLLER_TYPE_PS4:
    case SDL_CONTROLLER_TYPE_PS5:
        return LI_CTYPE_PS;
#ifdef SDL_CONTROLLER_TYPE_STEAMDECK
    case SDL_CONTROLLER_TYPE_STEAMDECK:
        return LI_CTYPE_STEAM;
#endif
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
        return LI_CTYPE_NINTENDO;
    default:
        return LI_CTYPE_UNKNOWN;
    }
}

void SdlInputHandler::sendControllerArrival(GamepadState* state)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (state == nullptr || state->controller == nullptr) {
        return;
    }

    uint32_t supportedButtonFlags = 0;
    for (int i = 0; i < (int)SDL_arraysize(k_ButtonMap); i++) {
        if (SDL_GameControllerHasButton(state->controller, (SDL_GameControllerButton)i)) {
            supportedButtonFlags |= k_ButtonMap[i];
        }
    }

    uint32_t hapticCaps = 0;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    hapticCaps |= SDL_GameControllerHasRumble(state->controller) ? ML_HAPTIC_GC_RUMBLE : 0;
    hapticCaps |= SDL_GameControllerHasRumbleTriggers(state->controller) ? ML_HAPTIC_GC_TRIGGER_RUMBLE : 0;
#endif

    uint32_t capabilities = 0;
    if (SDL_GameControllerGetBindForAxis(state->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT).bindType == SDL_CONTROLLER_BINDTYPE_AXIS ||
        SDL_GameControllerGetBindForAxis(state->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT).bindType == SDL_CONTROLLER_BINDTYPE_AXIS) {
        capabilities |= LI_CCAP_ANALOG_TRIGGERS;
    }
    if (hapticCaps & ML_HAPTIC_GC_RUMBLE) {
        capabilities |= LI_CCAP_RUMBLE;
    }
    if (hapticCaps & ML_HAPTIC_GC_TRIGGER_RUMBLE) {
        capabilities |= LI_CCAP_TRIGGER_RUMBLE;
    }
    if (SDL_GameControllerGetNumTouchpads(state->controller) > 0) {
        capabilities |= LI_CCAP_TOUCHPAD;
        if (SDL_GameControllerGetNumTouchpads(state->controller) > 1) {
            capabilities |= LI_CCAP_DUAL_TOUCHPAD;
        }
    }
    if (SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_ACCEL)) {
        capabilities |= LI_CCAP_ACCEL;
    }
    if (SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_GYRO)) {
        capabilities |= LI_CCAP_GYRO;
    }
    if (SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(state->controller)) != SDL_JOYSTICK_POWER_UNKNOWN || SDL_VERSION_ATLEAST(2, 24, 0)) {
        capabilities |= LI_CCAP_BATTERY_STATE;
    }
    if (SDL_GameControllerHasLED(state->controller)) {
        capabilities |= LI_CCAP_RGB_LED;
    }

    uint8_t type = moonlightControllerType(state->controller);

    if (state->hapticsAttached) capabilities |= LI_CCAP_HAPTICS_PCM;
    // Under the Apollo magic, bits 8-9 are the emulation mode; a dual-touchpad
    // pad's LI_CCAP_DUAL_TOUCHPAD would otherwise turn a DS4 request into DS5.
    capabilities = (capabilities & ~APOLLO_EXTENDED_EMULATION_MODE_MASK) |
                   apolloExtendedEmulationCaps(m_ControllerEmulationMode, state->controller, type);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Sending controller arrival: player %d type %u caps 0x%x buttons 0x%x",
                state->index, type, capabilities, supportedButtonFlags);
    LiSendControllerArrivalEvent(state->index, m_GamepadMask, type, supportedButtonFlags, capabilities);
#else
    Q_UNUSED(state);
#endif
}

void SdlInputHandler::initializeControllers()
{
    // The host uses the first arrival for a player until disconnect. In merged
    // mode announce an attached DualSense before the Deck's built-in controls.
    // Preserve enumeration order and numbering in multi-controller mode.
    SDL_PumpEvents();
    SDL_FlushEvent(SDL_CONTROLLERDEVICEADDED);
    const int count = SDL_NumJoysticks();
    for (int pass = 0; pass < (m_MultiController ? 1 : 2); ++pass) {
        for (int device = 0; device < count; ++device) {
            if (!SDL_IsGameController(device)) continue;
#if SDL_VERSION_ATLEAST(2, 0, 12)
            const bool preferred = SDL_GameControllerTypeForIndex(device) == SDL_CONTROLLER_TYPE_PS5;
#else
            const bool preferred = false;
#endif
            if (!m_MultiController && preferred != (pass == 0)) continue;
            SDL_ControllerDeviceEvent event {};
            event.type = SDL_CONTROLLERDEVICEADDED;
            event.which = device;
            handleControllerDeviceEvent(&event);
        }
    }
}

void SdlInputHandler::handleControllerDeviceEvent(SDL_ControllerDeviceEvent* event)
{
    GamepadState* state;

    if (event->type == SDL_CONTROLLERDEVICEADDED) {
        int i;
        const char* name;
        SDL_GameController* controller;
        const char* mapping;
        char guidStr[33];
        uint32_t hapticCaps;

        controller = SDL_GameControllerOpen(event->which);
        if (controller == NULL) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to open gamepad: %s",
                         SDL_GetError());
            return;
        }

        // SDL_CONTROLLERDEVICEADDED can be reported multiple times for the same
        // gamepad in rare cases, because SDL doesn't fixup the device index in
        // the SDL_CONTROLLERDEVICEADDED event if an unopened gamepad disappears
        // before we've processed the add event.
        for (int i = 0; i < MAX_GAMEPADS; i++) {
            if (m_GamepadState[i].controller == controller) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Received duplicate add event for controller index: %d",
                            event->which);
                SDL_GameControllerClose(controller);
                return;
            }
        }

        // We used to use SDL_GameControllerGetPlayerIndex() here but that
        // can lead to strange issues due to bugs in Windows where an Xbox
        // controller will join as player 2, even though no player 1 controller
        // is connected at all. This pretty much screws any attempt to use
        // the gamepad in single player games, so just assign them in order from 0.
        i = 0;

        for (; i < MAX_GAMEPADS; i++) {
            SDL_assert(m_GamepadState[i].controller != controller);
            if (m_GamepadState[i].controller == NULL) {
                // Found an empty slot
                break;
            }
        }

        if (i == MAX_GAMEPADS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "No open gamepad slots found!");
            SDL_GameControllerClose(controller);
            return;
        }

        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(controller)),
                                  guidStr, sizeof(guidStr));
        if (m_IgnoreDeviceGuids.contains(guidStr, Qt::CaseInsensitive))
        {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Skipping ignored device with GUID: %s",
                        guidStr);
            SDL_GameControllerClose(controller);
            return;
        }

        state = &m_GamepadState[i];
        if (m_MultiController) {
            state->index = i;

#if SDL_VERSION_ATLEAST(2, 0, 12)
            // This will change indicators on the controller to show the assigned
            // player index. For Xbox 360 controllers, that means updating the LED
            // ring to light up the corresponding quadrant for this player.
            SDL_GameControllerSetPlayerIndex(controller, state->index);
#endif
        }
        else {
            // Always player 1 in single controller mode
            state->index = 0;
        }

        state->controller = controller;
        state->jsId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(state->controller));
        state->batteryPercentage = -1;
        state->batteryState = LI_BATTERY_STATE_UNKNOWN;
        state->batteryWarningShown = false;

        hapticCaps = 0;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        hapticCaps |= SDL_GameControllerHasRumble(controller) ? ML_HAPTIC_GC_RUMBLE : 0;
        hapticCaps |= SDL_GameControllerHasRumbleTriggers(controller) ? ML_HAPTIC_GC_TRIGGER_RUMBLE : 0;
#elif SDL_VERSION_ATLEAST(2, 0, 9)
        // Perform a tiny rumbles to see if haptics are supported.
        // NB: We cannot use zeros for rumble intensity or SDL will not actually call the JS driver
        // and we'll get a (potentially false) success value returned.
        hapticCaps |= SDL_GameControllerRumble(controller, 1, 1, 1) == 0 ? ML_HAPTIC_GC_RUMBLE : 0;
#if SDL_VERSION_ATLEAST(2, 0, 14)
        hapticCaps |= SDL_GameControllerRumbleTriggers(controller, 1, 1, 1) == 0 ? ML_HAPTIC_GC_TRIGGER_RUMBLE : 0;
#endif
#else
        state->haptic = SDL_HapticOpenFromJoystick(SDL_GameControllerGetJoystick(state->controller));
        state->hapticEffectId = -1;
        state->hapticMethod = GAMEPAD_HAPTIC_METHOD_NONE;
        if (state->haptic != nullptr) {
            // Query for supported haptic effects
            hapticCaps = SDL_HapticQuery(state->haptic);
            hapticCaps |= SDL_HapticRumbleSupported(state->haptic) ?
                            ML_HAPTIC_SIMPLE_RUMBLE : 0;

            if ((SDL_HapticQuery(state->haptic) & SDL_HAPTIC_LEFTRIGHT) == 0) {
                if (SDL_HapticRumbleSupported(state->haptic)) {
                    if (SDL_HapticRumbleInit(state->haptic) == 0) {
                        state->hapticMethod = GAMEPAD_HAPTIC_METHOD_SIMPLERUMBLE;
                    }
                }
                if (state->hapticMethod == GAMEPAD_HAPTIC_METHOD_NONE) {
                    SDL_HapticClose(state->haptic);
                    state->haptic = nullptr;
                }
            } else {
                state->hapticMethod = GAMEPAD_HAPTIC_METHOD_LEFTRIGHT;
            }
        }
        else {
            hapticCaps = 0;
        }
#endif

        mapping = SDL_GameControllerMapping(state->controller);
        name = SDL_GameControllerName(state->controller);

        uint16_t vendorId = SDL_GameControllerGetVendor(state->controller);
        uint16_t productId = SDL_GameControllerGetProduct(state->controller);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Gamepad %d (player %d) is: %s (VID/PID: 0x%.4x/0x%.4x) (SDL type %d) (haptic capabilities: 0x%x) (mapping: %s -> %s)",
                    i,
                    state->index,
                    name != nullptr ? name : "<null>",
                    vendorId,
                    productId,
                    (int)SDL_GameControllerGetType(state->controller),
                    hapticCaps,
                    guidStr,
                    mapping != nullptr ? mapping : "<null>");

        if (SDL_GameControllerGetType(state->controller) == SDL_CONTROLLER_TYPE_PS5) {
            SDL_Joystick* joystick = SDL_GameControllerGetJoystick(state->controller);
            const char* path = SDL_JoystickPath(joystick);
            const bool bluetoothPath = path != nullptr &&
                (std::strstr(path, "00001124-0000-1000-8000-00805f9b34fb") != nullptr ||
                 std::strstr(path, "VID&0002054c") != nullptr);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "DualSense transport classification: %s path=%s",
                        bluetoothPath ? "Bluetooth" : "USB", path != nullptr ? path : "<null>");
            bindSelectedDualSenseAudio(state, bluetoothPath, path);
            state->dualSenseBluetooth = bluetoothPath;
            state->dualSenseHeadsetStateKnown = false;
            state->dualSenseHeadsetConnected = false;
            state->lastDualSenseHeadsetPollTime = 0;
            if (!bluetoothPath) {
                activateDualSenseAudio(state->controller, m_DualSenseAudioMode);
            }
            const char* serial = SDL_GameControllerGetSerial(state->controller);
            const uint16_t firmware = SDL_GameControllerGetFirmwareVersion(state->controller);

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "DualSense %d native HID ready: path=%s serial=%s firmware=0x%.4x "
                        "touchpads=%d accel=%d gyro=%d battery=%d rumble=%d triggerRumble=%d led=%d",
                        state->index,
                        path != nullptr ? path : "<unavailable>",
                        serial != nullptr ? serial : "<unavailable>",
                        firmware,
                        SDL_GameControllerGetNumTouchpads(state->controller),
                        SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_ACCEL),
                        SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_GYRO),
                        SDL_JoystickCurrentPowerLevel(joystick),
                        (hapticCaps & ML_HAPTIC_GC_RUMBLE) != 0,
                        (hapticCaps & ML_HAPTIC_GC_TRIGGER_RUMBLE) != 0,
                        SDL_GameControllerHasLED(state->controller));
        }
        if (mapping != nullptr) {
            SDL_free((void*)mapping);
        }

        state->hapticsAttached = DualSenseHaptics::attach(state->index, state->controller);
        #if SDL_VERSION_ATLEAST(2, 0, 14)
        if ((m_ControllerKbmMode || m_GyroStickEnabled) && SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_GYRO)) {
            state->gyroReportPeriodMs=4;SDL_GameControllerSetSensorEnabled(state->controller,SDL_SENSOR_GYRO,SDL_TRUE);
        }
        #endif
        if (m_ControllerKbmMode) {
            state->kbmOwner = this;
#if SDL_VERSION_ATLEAST(2, 0, 14)
            if (SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_GYRO)) { state->gyroReportPeriodMs = 4; SDL_GameControllerSetSensorEnabled(state->controller, SDL_SENSOR_GYRO, SDL_TRUE); }
#endif
            if (m_ControllerKbmContinuous) {
                state->kbmTimer = SDL_AddTimer(4, SdlInputHandler::controllerKbmTimerCallback, state);
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Controller %d is active in Extended keyboard and mouse mode", state->index);
            publishControllerStatus(state);
            return;
        }

        // Add this gamepad to the gamepad mask
        if (m_MultiController) {
            // NB: Don't assert that it's unset here because we will already
            // have the mask set for initially attached gamepads to avoid confusing
            // apps running on the host.
            m_GamepadMask |= (1 << state->index);
        }
        else {
            SDL_assert(m_GamepadMask == 0x1);
        }

        SDL_JoystickPowerLevel powerLevel = SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(state->controller));

#if SDL_VERSION_ATLEAST(2, 0, 14)
        const uint8_t type = moonlightControllerType(state->controller);

        // If this is a PlayStation controller that doesn't have a touchpad button mapped,
        // we'll allow the Select+PS button combo to act as the touchpad.
        state->clickpadButtonEmulationEnabled =
            SDL_GameControllerGetBindForButton(state->controller, SDL_CONTROLLER_BUTTON_TOUCHPAD).bindType == SDL_CONTROLLER_BINDTYPE_NONE &&
            type == LI_CTYPE_PS;

        sendControllerArrival(state);
#else

        // Send an empty event to tell the PC we've arrived
        sendGamepadState(state);
#endif

        // Send a power level if it's known at this time
        if (powerLevel != SDL_JOYSTICK_POWER_UNKNOWN) {
            sendGamepadBatteryState(state, powerLevel);
        }
        else publishControllerStatus(state);
    }
    else if (event->type == SDL_CONTROLLERDEVICEREMOVED) {
        state = findStateForGamepad(event->which);
        if (state != NULL) {
            if (state->dualSenseHeadsetConnected &&
                    m_SelectedDualSenseAudioIndex == state->index) {
                SdlAudioRenderer::setDualSenseHeadsetActive(false);
            }
            resetControllerChord(state);
            if (SDL_GameControllerGetType(state->controller) == SDL_CONTROLLER_TYPE_PS5) {
                unbindSelectedDualSenseAudio(state);
            }
            if (state->mouseEmulationTimer != 0) {
                Session::get()->notifyMouseEmulationMode(false);
                SDL_RemoveTimer(state->mouseEmulationTimer);
            }
            if (state->kbmTimer != 0) {
                SDL_RemoveTimer(state->kbmTimer);
                state->kbmTimer = 0;
            }
            releaseControllerKbmState(state);

            if (state->hapticsAttached) DualSenseHaptics::detach(state->index);
            SDL_GameControllerClose(state->controller);

#if !SDL_VERSION_ATLEAST(2, 0, 9)
            if (state->haptic != nullptr) {
                SDL_HapticClose(state->haptic);
            }
#endif

            // Remove this from the gamepad mask in MC-mode
            if (m_ControllerKbmMode) {
                // This controller was intentionally never exposed as a gamepad.
            }
            else if (m_MultiController) {
                SDL_assert(m_GamepadMask & (1 << state->index));
                m_GamepadMask &= ~(1 << state->index);
            }
            else {
                SDL_assert(m_GamepadMask == 0x1);
            }

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Gamepad %d is gone",
                        state->index);
            removeControllerStatus(state->jsId);

            // Send a final event to let the PC know this gamepad is gone
            if (!m_ControllerKbmMode) {
                LiSendMultiControllerEvent(state->index, m_GamepadMask,
                                           0, 0, 0, 0, 0, 0, 0);
            }

            // Clear all remaining state from this slot
            SDL_memset(state, 0, sizeof(*state));
        }
    }
}

void SdlInputHandler::handleJoystickArrivalEvent(SDL_JoyDeviceEvent* event)
{
    SDL_assert(event->type == SDL_JOYDEVICEADDED);

    if (!SDL_IsGameController(event->which)) {
        char guidStr[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(event->which),
                                  guidStr, sizeof(guidStr));
        const char* name = SDL_JoystickNameForIndex(event->which);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Joystick discovered with no mapping: %s %s",
                    name ? name : "<UNKNOWN>",
                    guidStr);
        SDL_Joystick* joy = SDL_JoystickOpen(event->which);
        if (joy != nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Number of axes: %d | Number of buttons: %d | Number of hats: %d",
                        SDL_JoystickNumAxes(joy), SDL_JoystickNumButtons(joy),
                        SDL_JoystickNumHats(joy));
            SDL_JoystickClose(joy);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to open joystick for query: %s",
                        SDL_GetError());
        }
    }
}

void SdlInputHandler::rumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor)
{
    // Native waveform playback takes precedence over legacy rumble.
    if (DualSenseHaptics::playing(controllerNumber)) return;
    // Make sure the controller number is within our supported count
    if (controllerNumber >= MAX_GAMEPADS) {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 9)
    // Host controllerNumber is the player index, not the SDL slot. In
    // single-controller mode the Deck occupies slot 0 and DualSense slot 1
    // while both report player 0, so indexing by slot would rumble the Deck.
    bool preferPs5 = false;
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (m_GamepadState[i].controller != nullptr &&
            m_GamepadState[i].index == (short)controllerNumber &&
            SDL_GameControllerGetType(m_GamepadState[i].controller) == SDL_CONTROLLER_TYPE_PS5) {
            preferPs5 = true;
            break;
        }
    }

    bool routed = false;
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        GamepadState* state = &m_GamepadState[i];
        if (state->controller == nullptr || state->index != (short)controllerNumber) {
            continue;
        }
        if (preferPs5 && SDL_GameControllerGetType(state->controller) != SDL_CONTROLLER_TYPE_PS5) {
            continue;
        }

        routed = true;
        if (DualSenseHaptics::playing(controllerNumber)) continue;
        if (SDL_GameControllerGetType(state->controller) == SDL_CONTROLLER_TYPE_PS5 &&
                m_SelectedDualSenseAudioIndex == state->index &&
                DualSenseAudioRenderer::instance().isBluetooth()) {
            DualSenseAudioRenderer::instance().setBluetoothRumble(lowFreqMotor, highFreqMotor);
            continue;
        }
        const int rc = SDL_GameControllerRumble(state->controller, lowFreqMotor, highFreqMotor, 30000);

        static uint32_t lastRumbleLogMs;
        const uint32_t now = SDL_GetTicks();
        if (rc != 0 || lastRumbleLogMs == 0 || SDL_TICKS_PASSED(now, lastRumbleLogMs + 2000)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Rumble player %d slot %d %s motors %u/%u rc %d%s",
                        controllerNumber,
                        i,
                        SDL_GameControllerName(state->controller),
                        lowFreqMotor,
                        highFreqMotor,
                        rc,
                        rc != 0 ? SDL_GetError() : "");
            lastRumbleLogMs = now;
        }
    }

    if (!routed && (lowFreqMotor | highFreqMotor) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Rumble for player %d had no matching gamepad",
                    controllerNumber);
    }
#else
    // Check if the controller supports haptics (and if the controller exists at all)
    SDL_Haptic* haptic = m_GamepadState[controllerNumber].haptic;
    if (haptic == nullptr) {
        return;
    }

    // Stop the last effect we played
    if (m_GamepadState[controllerNumber].hapticMethod == GAMEPAD_HAPTIC_METHOD_LEFTRIGHT) {
        if (m_GamepadState[controllerNumber].hapticEffectId >= 0) {
            SDL_HapticDestroyEffect(haptic, m_GamepadState[controllerNumber].hapticEffectId);
        }
    } else if (m_GamepadState[controllerNumber].hapticMethod == GAMEPAD_HAPTIC_METHOD_SIMPLERUMBLE) {
        SDL_HapticRumbleStop(haptic);
    }

    // If this callback is telling us to stop both motors, don't bother queuing a new effect
    if (lowFreqMotor == 0 && highFreqMotor == 0) {
        return;
    }

    if (m_GamepadState[controllerNumber].hapticMethod == GAMEPAD_HAPTIC_METHOD_LEFTRIGHT) {
        SDL_HapticEffect effect;
        SDL_memset(&effect, 0, sizeof(effect));
        effect.type = SDL_HAPTIC_LEFTRIGHT;

        // The effect should last until we are instructed to stop or change it
        effect.leftright.length = SDL_HAPTIC_INFINITY;

        // SDL haptics range from 0-32767 but XInput uses 0-65535, so divide by 2 to correct for SDL's scaling
        effect.leftright.large_magnitude = lowFreqMotor / 2;
        effect.leftright.small_magnitude = highFreqMotor / 2;

        // Play the new effect
        m_GamepadState[controllerNumber].hapticEffectId = SDL_HapticNewEffect(haptic, &effect);
        if (m_GamepadState[controllerNumber].hapticEffectId >= 0) {
            SDL_HapticRunEffect(haptic, m_GamepadState[controllerNumber].hapticEffectId, 1);
        }
    } else if (m_GamepadState[controllerNumber].hapticMethod == GAMEPAD_HAPTIC_METHOD_SIMPLERUMBLE) {
        SDL_HapticRumblePlay(haptic,
                             std::min(1.0, (GAMEPAD_HAPTIC_SIMPLE_HIFREQ_MOTOR_WEIGHT*highFreqMotor +
                                            GAMEPAD_HAPTIC_SIMPLE_LOWFREQ_MOTOR_WEIGHT*lowFreqMotor) / 65535.0),
                             SDL_HAPTIC_INFINITY);
    }
#endif
}

void SdlInputHandler::rumbleTriggers(uint16_t controllerNumber, uint16_t leftTrigger, uint16_t rightTrigger)
{
    // Make sure the controller number is within our supported count
    if (controllerNumber >= MAX_GAMEPADS) {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 14)
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        GamepadState* state = &m_GamepadState[i];
        if (state->controller == nullptr || state->index != (short)controllerNumber) {
            continue;
        }
        const int rc = SDL_GameControllerRumbleTriggers(state->controller, leftTrigger, rightTrigger, 30000);
        if (rc != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Trigger rumble player %d slot %d rc %d: %s",
                        controllerNumber, i, rc, SDL_GetError());
        }
    }
#endif
}

void SdlInputHandler::setMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz)
{
    // Make sure the controller number is within our supported count
    if (controllerNumber >= MAX_GAMEPADS) {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 14)
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        GamepadState* state = &m_GamepadState[i];
        if (state->controller == nullptr || state->index != (short)controllerNumber) {
            continue;
        }

        uint8_t reportPeriodMs = reportRateHz ? (1000 / reportRateHz) : 0;
        switch (motionType) {
        case LI_MOTION_TYPE_ACCEL:
            state->accelReportPeriodMs = reportPeriodMs;
            SDL_GameControllerSetSensorEnabled(state->controller, SDL_SENSOR_ACCEL, reportRateHz ? SDL_TRUE : SDL_FALSE);
            break;

        case LI_MOTION_TYPE_GYRO:
            state->gyroReportPeriodMs = reportPeriodMs;
            SDL_GameControllerSetSensorEnabled(state->controller, SDL_SENSOR_GYRO, reportRateHz ? SDL_TRUE : SDL_FALSE);
            break;
        }
    }
#endif
}

void SdlInputHandler::setControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b)
{
    // Make sure the controller number is within our supported count
    if (controllerNumber >= MAX_GAMEPADS) {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 14)
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        GamepadState* state = &m_GamepadState[i];
        if (state->controller == nullptr || state->index != (short)controllerNumber) {
            continue;
        }
        if (SDL_GameControllerGetType(state->controller) == SDL_CONTROLLER_TYPE_PS5 &&
                m_SelectedDualSenseAudioIndex == state->index &&
                DualSenseAudioRenderer::instance().isBluetooth()) {
            DualSenseAudioRenderer::instance().setBluetoothLed(r, g, b);
            continue;
        }
        SDL_GameControllerSetLED(state->controller, r, g, b);
    }
#endif
}

void SdlInputHandler::setAdaptiveTriggers(uint16_t controllerNumber, DualSenseOutputReport *report)
{
#if SDL_VERSION_ATLEAST(2, 0, 16)
    if (controllerNumber < MAX_GAMEPADS && report != nullptr) {
        bool sent = false;
        for (int i = 0; i < MAX_GAMEPADS; i++) {
            GamepadState* state = &m_GamepadState[i];
            if (state->controller == nullptr ||
                state->index != (short)controllerNumber ||
                SDL_GameControllerGetType(state->controller) != SDL_CONTROLLER_TYPE_PS5) {
                continue;
            }

            if (m_SelectedDualSenseAudioIndex == state->index &&
                    DualSenseAudioRenderer::instance().isBluetooth()) {
                DualSenseAudioRenderer::instance().setBluetoothTriggers(report);
                sent = true;
                continue;
            }

            // Player LED fields are only populated when the host sets the
            // player-indicator flag, so USB also carries host player LEDs.
            const int rc = SDL_GameControllerSendEffect(state->controller, report, sizeof(*report));
            sent = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Adaptive triggers player %d slot %d flags 0x%x types %u/%u rc %d%s",
                        controllerNumber,
                        i,
                        report->validFlag0,
                        report->leftTriggerEffectType,
                        report->rightTriggerEffectType,
                        rc,
                        rc != 0 ? SDL_GetError() : "");
        }
        if (!sent) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Adaptive triggers for player %d had no PS5 gamepad",
                        controllerNumber);
        }
    }
#endif
    SDL_free(report);
}

QString SdlInputHandler::getUnmappedGamepads()
{
    QString ret;

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) failed: %s",
                     SDL_GetError());
    }

    MappingManager mappingManager;
    mappingManager.applyMappings();

    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (!SDL_IsGameController(i)) {
            char guidStr[33];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i),
                                      guidStr, sizeof(guidStr));
            const char* name = SDL_JoystickNameForIndex(i);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Unmapped joystick: %s %s",
                        name ? name : "<UNKNOWN>",
                        guidStr);
            SDL_Joystick* joy = SDL_JoystickOpen(i);
            if (joy != nullptr) {
                int numButtons = SDL_JoystickNumButtons(joy);
                int numHats = SDL_JoystickNumHats(joy);
                int numAxes = SDL_JoystickNumAxes(joy);

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Number of axes: %d | Number of buttons: %d | Number of hats: %d",
                            numAxes, numButtons, numHats);

                if ((numAxes >= 4 && numAxes <= 8) && numButtons >= 8 && numHats <= 1) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Joystick likely to be an unmapped game controller");
                    if (!ret.isEmpty()) {
                        ret += ", ";
                    }

                    ret += name;
                }

                SDL_JoystickClose(joy);
            }
            else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Unable to open joystick for query: %s",
                            SDL_GetError());
            }
        }
    }

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);

    // Flush stale events so they aren't processed by the main session event loop
    SDL_FlushEvents(SDL_JOYDEVICEADDED, SDL_JOYDEVICEREMOVED);
    SDL_FlushEvents(SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEREMAPPED);

    return ret;
}

int SdlInputHandler::getAttachedGamepadMask()
{
    int count;
    int mask;

    if (m_ControllerKbmMode) {
        return 0;
    }
    if (!m_MultiController) {
        // Player 1 is always present in non-MC mode
        return 0x1;
    }

    count = mask = 0;
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            char guidStr[33];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i),
                                      guidStr, sizeof(guidStr));

            if (!m_IgnoreDeviceGuids.contains(guidStr, Qt::CaseInsensitive))
            {
                mask |= (1 << count++);
            }
        }
    }

    return mask;
}

int SdlInputHandler::getAttachedPlayStationGamepadMask()
{
    int count = 0;
    int mask = 0;
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (!SDL_IsGameController(i)) {
            continue;
        }

        char guidStr[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i),
                                  guidStr, sizeof(guidStr));
        if (m_IgnoreDeviceGuids.contains(guidStr, Qt::CaseInsensitive)) {
            continue;
        }

        bool isPlayStationController;
#if SDL_VERSION_ATLEAST(2, 0, 12)
        const auto type = SDL_GameControllerTypeForIndex(i);
        isPlayStationController = type == SDL_CONTROLLER_TYPE_PS3 ||
                                  type == SDL_CONTROLLER_TYPE_PS4;
#if SDL_VERSION_ATLEAST(2, 0, 14)
        isPlayStationController = isPlayStationController ||
                                  type == SDL_CONTROLLER_TYPE_PS5;
#endif
#else
        isPlayStationController = SDL_JoystickGetDeviceVendor(i) == 0x054c;
#endif
        if (isPlayStationController && (!m_MultiController || count < MAX_GAMEPADS)) {
            mask |= 1 << (m_MultiController ? count : 0);
        }
        count++;
    }

    return mask;
}
