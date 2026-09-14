import Carbon.HIToolbox
import Foundation

/// What ⌘ becomes on the Pi.
public enum CommandKey: String, Codable, Sendable, CaseIterable {
    /// Super (the Logo key): MaryOS's apps take their chords as Ctrl-or-Logo, and Spotlight opens on
    /// Super+Space, while the terminal keeps Ctrl for itself.
    case `super`
    /// Control, for Wayland clients such as foot that only know Ctrl chords.
    case control
}

/// Mac virtual key codes (`kVK_*`) to Linux evdev key codes, so the Pi's own keymap turns them into
/// characters. Position, not character: ⌥ is Alt, Delete is Backspace, Help is Insert.
public struct KeyMap: Sendable {
    public var commandKey: CommandKey

    public init(commandKey: CommandKey = .super) {
        self.commandKey = commandKey
    }

    public func evdev(forKeyCode keyCode: UInt16) -> UInt16? {
        switch Int(keyCode) {
        case kVK_Command: commandKey == .super ? 125 : 29      // KEY_LEFTMETA, KEY_LEFTCTRL
        case kVK_RightCommand: commandKey == .super ? 126 : 97 // KEY_RIGHTMETA, KEY_RIGHTCTRL
        default: Self.table[Int(keyCode)]
        }
    }

    /// Keys that arrive as flag changes rather than key downs and ups.
    public static let modifierKeyCodes: Set<UInt16> = Set([
        kVK_Command, kVK_RightCommand, kVK_Shift, kVK_RightShift, kVK_Option, kVK_RightOption,
        kVK_Control, kVK_RightControl, kVK_CapsLock, kVK_Function,
    ].map { UInt16($0) })

    static let table: [Int: UInt16] = [
        kVK_ANSI_A: 30, kVK_ANSI_S: 31, kVK_ANSI_D: 32, kVK_ANSI_F: 33, kVK_ANSI_H: 35, kVK_ANSI_G: 34,
        kVK_ANSI_Z: 44, kVK_ANSI_X: 45, kVK_ANSI_C: 46, kVK_ANSI_V: 47, kVK_ISO_Section: 86, kVK_ANSI_B: 48,
        kVK_ANSI_Q: 16, kVK_ANSI_W: 17, kVK_ANSI_E: 18, kVK_ANSI_R: 19, kVK_ANSI_Y: 21, kVK_ANSI_T: 20,
        kVK_ANSI_1: 2, kVK_ANSI_2: 3, kVK_ANSI_3: 4, kVK_ANSI_4: 5, kVK_ANSI_6: 7, kVK_ANSI_5: 6,
        kVK_ANSI_Equal: 13, kVK_ANSI_9: 10, kVK_ANSI_7: 8, kVK_ANSI_Minus: 12, kVK_ANSI_8: 9, kVK_ANSI_0: 11,
        kVK_ANSI_RightBracket: 27, kVK_ANSI_O: 24, kVK_ANSI_U: 22, kVK_ANSI_LeftBracket: 26, kVK_ANSI_I: 23,
        kVK_ANSI_P: 25, kVK_Return: 28, kVK_ANSI_L: 38, kVK_ANSI_J: 36, kVK_ANSI_Quote: 40, kVK_ANSI_K: 37,
        kVK_ANSI_Semicolon: 39, kVK_ANSI_Backslash: 43, kVK_ANSI_Comma: 51, kVK_ANSI_Slash: 53, kVK_ANSI_N: 49,
        kVK_ANSI_M: 50, kVK_ANSI_Period: 52, kVK_Tab: 15, kVK_Space: 57, kVK_ANSI_Grave: 41, kVK_Delete: 14,
        kVK_Escape: 1,
        kVK_Shift: 42, kVK_CapsLock: 58, kVK_Option: 56, kVK_Control: 29, kVK_RightShift: 54, kVK_RightOption: 100,
        kVK_RightControl: 97,
        kVK_ANSI_KeypadDecimal: 83, kVK_ANSI_KeypadMultiply: 55, kVK_ANSI_KeypadPlus: 78, kVK_ANSI_KeypadClear: 69,
        kVK_ANSI_KeypadDivide: 98, kVK_ANSI_KeypadEnter: 96, kVK_ANSI_KeypadMinus: 74, kVK_ANSI_KeypadEquals: 117,
        kVK_ANSI_Keypad0: 82, kVK_ANSI_Keypad1: 79, kVK_ANSI_Keypad2: 80, kVK_ANSI_Keypad3: 81, kVK_ANSI_Keypad4: 75,
        kVK_ANSI_Keypad5: 76, kVK_ANSI_Keypad6: 77, kVK_ANSI_Keypad7: 71, kVK_ANSI_Keypad8: 72, kVK_ANSI_Keypad9: 73,
        kVK_VolumeUp: 115, kVK_VolumeDown: 114, kVK_Mute: 113,
        kVK_JIS_Yen: 124, kVK_JIS_Underscore: 89, kVK_JIS_KeypadComma: 121, kVK_JIS_Eisu: 123, kVK_JIS_Kana: 122,
        kVK_F1: 59, kVK_F2: 60, kVK_F3: 61, kVK_F4: 62, kVK_F5: 63, kVK_F6: 64, kVK_F7: 65, kVK_F8: 66, kVK_F9: 67,
        kVK_F10: 68, kVK_F11: 87, kVK_F12: 88, kVK_F13: 183, kVK_F14: 184, kVK_F15: 185, kVK_F16: 186, kVK_F17: 187,
        kVK_F18: 188, kVK_F19: 189, kVK_F20: 190,
        kVK_Help: 110, kVK_Home: 102, kVK_PageUp: 104, kVK_ForwardDelete: 111, kVK_End: 107, kVK_PageDown: 109,
        kVK_LeftArrow: 105, kVK_RightArrow: 106, kVK_DownArrow: 108, kVK_UpArrow: 103,
    ]
}
