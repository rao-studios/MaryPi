import Foundation

/// The pointer buttons held, as `pointer` carries them.
public struct PointerButtons: OptionSet, Hashable, Sendable {
    public let rawValue: UInt8

    public init(rawValue: UInt8) {
        self.rawValue = rawValue
    }

    public static let left = PointerButtons(rawValue: 1)
    public static let right = PointerButtons(rawValue: 2)
    public static let middle = PointerButtons(rawValue: 4)
}

/// `quality`: how the Pi encodes its JPEGs.
public enum FrameQuality: UInt8, Codable, Sendable, CaseIterable {
    /// 4:4:4 at quality 85.
    case best = 0
    /// 4:2:0 at quality 75, for a slow link.
    case fast = 1
}

/// One JPEG rectangle of a `frame`, in desktop pixels.
public struct FrameRect: Equatable, Sendable {
    public static let codecJPEG: UInt8 = 1

    public var x: UInt16
    public var y: UInt16
    public var width: UInt16
    public var height: UInt16
    public var jpeg: [UInt8]

    public init(x: UInt16, y: UInt16, width: UInt16, height: UInt16, jpeg: [UInt8]) {
        self.x = x
        self.y = y
        self.width = width
        self.height = height
        self.jpeg = jpeg
    }
}

/// One session message, in either direction (`maryvnc/include/maryvnc/wire.h`). The type byte comes first;
/// decoding is strict, so a message is exactly its fields.
public enum WireMessage: Equatable, Sendable {
    // Pi → Mac
    case hello(version: UInt16, name: String, width: UInt16, height: UInt16, fingerprint: [UInt8])
    case frame(seq: UInt32, rects: [FrameRect])
    /// `shown`: the pointer is a hardware plane on the Pi, not in the pixels, so the viewer draws one.
    case cursor(x: Int16, y: Int16, shown: Bool)
    case serverBye(reason: String)
    // Mac → Pi
    case pointer(x: UInt16, y: UInt16, buttons: PointerButtons, scrollX: Int16, scrollY: Int16)
    case key(evdev: UInt16, pressed: Bool)
    case ack(seq: UInt32)
    case quality(FrameQuality)
    case refresh
    case viewerBye

    enum Kind: UInt8 {
        case hello = 0x01, frame = 0x02, cursor = 0x03, serverBye = 0x04
        case pointer = 0x10, key = 0x11, ack = 0x12, quality = 0x13, refresh = 0x14, viewerBye = 0x15
    }

    public var isFromServer: Bool {
        switch self {
        case .hello, .frame, .cursor, .serverBye: true
        default: false
        }
    }

    public func encoded() throws -> [UInt8] {
        var w = ByteWriter()
        switch self {
        case let .hello(version, name, width, height, fingerprint):
            guard fingerprint.count == 32 else { throw MaryVNCError("a fingerprint is 32 bytes") }
            w.u8(Kind.hello.rawValue)
            w.u16(version)
            try w.string(name, max: MaryVNC.nameMax)
            w.u16(width)
            w.u16(height)
            w.raw(fingerprint)
        case let .frame(seq, rects):
            guard rects.count <= MaryVNC.rectsMax else { throw MaryVNCError("a frame of more than \(MaryVNC.rectsMax) rectangles") }
            w.u8(Kind.frame.rawValue)
            w.u32(seq)
            w.u16(UInt16(rects.count))
            for rect in rects {
                guard w.bytes.count + 13 + rect.jpeg.count <= MaryVNC.messageMax else { throw MaryVNCError("a frame past the message limit") }
                w.u16(rect.x)
                w.u16(rect.y)
                w.u16(rect.width)
                w.u16(rect.height)
                w.u8(FrameRect.codecJPEG)
                w.u32(UInt32(rect.jpeg.count))
                w.raw(rect.jpeg)
            }
        case let .cursor(x, y, shown):
            w.u8(Kind.cursor.rawValue)
            w.i16(x)
            w.i16(y)
            w.u8(shown ? 1 : 0)
        case let .serverBye(reason):
            w.u8(Kind.serverBye.rawValue)
            try w.string(reason, max: MaryVNC.reasonMax)
        case let .pointer(x, y, buttons, scrollX, scrollY):
            guard buttons.rawValue <= 7 else { throw MaryVNCError("pointer buttons past middle") }
            w.u8(Kind.pointer.rawValue)
            w.u16(x)
            w.u16(y)
            w.u8(buttons.rawValue)
            w.i16(scrollX)
            w.i16(scrollY)
        case let .key(evdev, pressed):
            w.u8(Kind.key.rawValue)
            w.u16(evdev)
            w.u8(pressed ? 1 : 0)
        case let .ack(seq):
            w.u8(Kind.ack.rawValue)
            w.u32(seq)
        case let .quality(level):
            w.u8(Kind.quality.rawValue)
            w.u8(level.rawValue)
        case .refresh:
            w.u8(Kind.refresh.rawValue)
        case .viewerBye:
            w.u8(Kind.viewerBye.rawValue)
        }
        guard w.bytes.count <= MaryVNC.messageMax else { throw MaryVNCError("a message past the message limit") }
        return w.bytes
    }

    public static func decode(_ bytes: [UInt8]) throws -> WireMessage {
        guard bytes.count <= MaryVNC.messageMax else { throw MaryVNCError("a message past the message limit") }
        var r = ByteReader(bytes)
        guard let kind = Kind(rawValue: try r.u8()) else { throw MaryVNCError("a message of a type this viewer does not know") }
        let message: WireMessage
        switch kind {
        case .hello:
            let version = try r.u16()
            let name = try r.string(max: MaryVNC.nameMax)
            message = .hello(version: version, name: name, width: try r.u16(), height: try r.u16(), fingerprint: try r.take(32))
        case .frame:
            let seq = try r.u32()
            let count = Int(try r.u16())
            guard count <= MaryVNC.rectsMax else { throw MaryVNCError("a frame of more than \(MaryVNC.rectsMax) rectangles") }
            var rects: [FrameRect] = []
            for _ in 0..<count {
                let x = try r.u16(), y = try r.u16(), width = try r.u16(), height = try r.u16()
                guard try r.u8() == FrameRect.codecJPEG else { throw MaryVNCError("a rectangle in a codec other than JPEG") }
                let length = Int(try r.u32())
                rects.append(FrameRect(x: x, y: y, width: width, height: height, jpeg: try r.take(length)))
            }
            message = .frame(seq: seq, rects: rects)
        case .cursor:
            let x = try r.i16(), y = try r.i16(), shown = try r.u8()
            guard shown <= 1 else { throw MaryVNCError("a cursor flag out of range") }
            message = .cursor(x: x, y: y, shown: shown == 1)
        case .serverBye:
            message = .serverBye(reason: try r.string(max: MaryVNC.reasonMax))
        case .pointer:
            let x = try r.u16(), y = try r.u16(), buttons = try r.u8(), scrollX = try r.i16(), scrollY = try r.i16()
            guard buttons <= 7 else { throw MaryVNCError("pointer buttons past middle") }
            message = .pointer(x: x, y: y, buttons: PointerButtons(rawValue: buttons), scrollX: scrollX, scrollY: scrollY)
        case .key:
            let evdev = try r.u16(), pressed = try r.u8()
            guard pressed <= 1 else { throw MaryVNCError("a key flag out of range") }
            message = .key(evdev: evdev, pressed: pressed == 1)
        case .ack:
            message = .ack(seq: try r.u32())
        case .quality:
            guard let level = FrameQuality(rawValue: try r.u8()) else { throw MaryVNCError("a quality level out of range") }
            message = .quality(level)
        case .refresh:
            message = .refresh
        case .viewerBye:
            message = .viewerBye
        }
        try r.finish()
        return message
    }
}

/// The payload of every handshake message: a version, and the Mac's display name in the viewer's last
/// handshake message (empty in the others).
public struct HandshakePayload: Equatable, Sendable {
    public var version: UInt16
    public var name: String

    public init(version: UInt16 = MaryVNC.version, name: String = "") {
        self.version = version
        self.name = name
    }

    public func encoded() throws -> [UInt8] {
        var w = ByteWriter()
        w.u16(version)
        try w.string(name, max: MaryVNC.nameMax)
        return w.bytes
    }

    public static func decode(_ bytes: [UInt8]) throws -> HandshakePayload {
        var r = ByteReader(bytes)
        let payload = HandshakePayload(version: try r.u16(), name: try r.string(max: MaryVNC.nameMax))
        try r.finish()
        return payload
    }
}
