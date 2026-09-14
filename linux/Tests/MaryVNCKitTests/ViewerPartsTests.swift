import Carbon.HIToolbox
import CoreGraphics
import Foundation
import ImageIO
import Testing
@testable import MaryVNCKit

@Suite struct KeyMapTests {
    @Test func lettersDigitsAndEditingKeysLandOnTheirEvdevCodes() {
        let map = KeyMap()
        #expect(map.evdev(forKeyCode: UInt16(kVK_ANSI_A)) == 30)
        #expect(map.evdev(forKeyCode: UInt16(kVK_ANSI_Q)) == 16)
        #expect(map.evdev(forKeyCode: UInt16(kVK_ANSI_1)) == 2)
        #expect(map.evdev(forKeyCode: UInt16(kVK_ANSI_0)) == 11)
        #expect(map.evdev(forKeyCode: UInt16(kVK_Delete)) == 14)          // KEY_BACKSPACE
        #expect(map.evdev(forKeyCode: UInt16(kVK_ForwardDelete)) == 111)  // KEY_DELETE
        #expect(map.evdev(forKeyCode: UInt16(kVK_Space)) == 57)
        #expect(map.evdev(forKeyCode: UInt16(kVK_UpArrow)) == 103)
        #expect(map.evdev(forKeyCode: UInt16(kVK_Function)) == nil)
    }

    @Test func commandIsSuperOrControl() {
        #expect(KeyMap(commandKey: .super).evdev(forKeyCode: UInt16(kVK_Command)) == 125)
        #expect(KeyMap(commandKey: .super).evdev(forKeyCode: UInt16(kVK_RightCommand)) == 126)
        #expect(KeyMap(commandKey: .control).evdev(forKeyCode: UInt16(kVK_Command)) == 29)
        #expect(KeyMap(commandKey: .control).evdev(forKeyCode: UInt16(kVK_RightCommand)) == 97)
    }

    @Test func everyLetterFunctionKeyAndModifierIsMappedAndNoTwoKeysCollide() {
        let letters = [kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F, kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I,
                       kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R,
                       kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z]
        let functions = [kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6, kVK_F7, kVK_F8, kVK_F9, kVK_F10, kVK_F11, kVK_F12]
        let map = KeyMap()
        #expect(Set(letters.compactMap { map.evdev(forKeyCode: UInt16($0)) }).count == 26)
        #expect(Set(functions.compactMap { map.evdev(forKeyCode: UInt16($0)) }).count == 12)
        for code in KeyMap.modifierKeyCodes where Int(code) != kVK_Function {
            #expect(map.evdev(forKeyCode: code) != nil, "modifier \(code)")
        }
        let all = (0..<128).compactMap { map.evdev(forKeyCode: UInt16($0)) }
        #expect(Set(all).count == all.count)
        #expect(all.allSatisfy { $0 < 768 })
    }
}

@Suite struct FramebufferTests {
    private func jpeg(width: Int, height: Int, red: CGFloat, green: CGFloat, blue: CGFloat) throws -> [UInt8] {
        let space = try #require(CGColorSpace(name: CGColorSpace.sRGB))
        let context = try #require(CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                                             bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue))
        context.setFillColor(red: red, green: green, blue: blue, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        let image = try #require(context.makeImage())
        let data = NSMutableData()
        let destination = try #require(CGImageDestinationCreateWithData(data, "public.jpeg" as CFString, 1, nil))
        CGImageDestinationAddImage(destination, image, [kCGImageDestinationLossyCompressionQuality: 1.0] as CFDictionary)
        #expect(CGImageDestinationFinalize(destination))
        return Array(data as Data)
    }

    @Test func aJPEGRectangleLandsWhereTheFrameSays() throws {
        let fb = try Framebuffer(width: 64, height: 32)
        try fb.apply(FrameRect(x: 8, y: 4, width: 32, height: 16, jpeg: jpeg(width: 32, height: 16, red: 1, green: 0, blue: 0)))
        for (x, y) in [(8, 4), (39, 19), (20, 10)] {
            let p = try #require(fb.pixel(x: x, y: y))
            #expect(p.red > 240 && p.green < 16 && p.blue < 16, "inside at \(x),\(y): \(p)")
        }
        for (x, y) in [(7, 4), (40, 4), (8, 3), (8, 20), (0, 0)] {
            let p = try #require(fb.pixel(x: x, y: y))
            #expect(p.red == 0 && p.green == 0 && p.blue == 0, "outside at \(x),\(y): \(p)")
        }
        let image = try #require(fb.makeImage())
        #expect(image.width == 64 && image.height == 32)
    }

    @Test func rectanglesThatDoNotFitOrDecodeAreRefused() throws {
        let fb = try Framebuffer(width: 64, height: 32)
        let blue = try jpeg(width: 16, height: 16, red: 0, green: 0, blue: 1)
        #expect(throws: MaryVNCError.self) { try fb.apply(FrameRect(x: 56, y: 0, width: 16, height: 16, jpeg: blue)) }
        #expect(throws: MaryVNCError.self) { try fb.apply(FrameRect(x: 0, y: 0, width: 32, height: 16, jpeg: blue)) }
        #expect(throws: MaryVNCError.self) { try fb.apply(FrameRect(x: 0, y: 0, width: 16, height: 16, jpeg: [0xff, 0xd8, 1, 2])) }
        #expect(throws: MaryVNCError.self) { try Framebuffer(width: 0, height: 10) }
    }
}

@Suite struct DiscoveryTests {
    @Test func theCablesNetworkIsTenTwelveOneNinetyFourSlashTwentyEight() {
        #expect(USBLink.contains(0x0a0c_c201))   // 10.12.194.1, the Pi
        #expect(USBLink.contains(0x0a0c_c20e))   // .14
        #expect(!USBLink.contains(0x0a0c_c210))  // .16
        #expect(!USBLink.contains(0x0a0c_c302))  // 10.12.195.2
        #expect(!USBLink.contains(0xc0a8_0102))  // 192.168.1.2
    }

    @Test func theTXTRecordGivesTheClaimedKeyAndVersion() {
        let id = String(repeating: "ab", count: 32)
        let parsed = DiscoveredPi.parseTXT(["id": id, "v": "1"])
        #expect(parsed.fingerprint?.hex == id)
        #expect(parsed.version == 1)
        #expect(DiscoveredPi.parseTXT(["id": "abc", "v": "one"]).fingerprint == nil)
        #expect(DiscoveredPi.parseTXT([:]).version == nil)
    }
}
