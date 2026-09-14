import Foundation

/// Little-endian fields out, as `maryvnc/src/wire.c` writes them.
struct ByteWriter {
    private(set) var bytes: [UInt8] = []

    mutating func u8(_ value: UInt8) { bytes.append(value) }
    mutating func u16(_ value: UInt16) { bytes += [UInt8(value & 0xff), UInt8(value >> 8)] }
    mutating func i16(_ value: Int16) { u16(UInt16(bitPattern: value)) }
    mutating func u32(_ value: UInt32) { bytes += (0..<4).map { UInt8((value >> (8 * UInt32($0))) & 0xff) } }
    mutating func raw(_ data: [UInt8]) { bytes += data }

    /// A `str`: a u16 length, then UTF-8 with no control characters, at most `max` bytes.
    mutating func string(_ value: String, max: Int) throws {
        let utf8 = Array(value.utf8)
        guard utf8.count <= max, WireText.isValid(utf8) else { throw MaryVNCError("a name or reason the wire does not allow") }
        u16(UInt16(utf8.count))
        raw(utf8)
    }
}

/// Little-endian fields in; anything short throws, and `finish` refuses trailing bytes.
struct ByteReader {
    private let bytes: [UInt8]
    private var at = 0

    init(_ bytes: [UInt8]) {
        self.bytes = bytes
    }

    mutating func take(_ count: Int) throws -> [UInt8] {
        guard count >= 0, bytes.count - at >= count else { throw MaryVNCError("a message shorter than its fields") }
        defer { at += count }
        return Array(bytes[at..<at + count])
    }

    mutating func u8() throws -> UInt8 { try take(1)[0] }

    mutating func u16() throws -> UInt16 {
        let b = try take(2)
        return UInt16(b[0]) | UInt16(b[1]) << 8
    }

    mutating func i16() throws -> Int16 { Int16(bitPattern: try u16()) }

    mutating func u32() throws -> UInt32 {
        let b = try take(4)
        return UInt32(b[0]) | UInt32(b[1]) << 8 | UInt32(b[2]) << 16 | UInt32(b[3]) << 24
    }

    mutating func string(max: Int) throws -> String {
        let count = Int(try u16())
        guard count <= max else { throw MaryVNCError("a name or reason longer than the wire allows") }
        let utf8 = try take(count)
        guard WireText.isValid(utf8) else { throw MaryVNCError("a name or reason with bytes the wire does not allow") }
        return String(decoding: utf8, as: UTF8.self)
    }

    func finish() throws {
        guard at == bytes.count else { throw MaryVNCError("a message longer than its fields") }
    }
}

/// `mv_wire_text_valid`: valid UTF-8 (no overlong forms, no surrogates) with no C0 or C1 control
/// characters and no DEL.
enum WireText {
    static func isValid(_ bytes: [UInt8]) -> Bool {
        var i = 0
        while i < bytes.count {
            let c = UInt32(bytes[i])
            if c < 0x20 || c == 0x7f { return false }
            if c < 0x80 {
                i += 1
                continue
            }
            let count: Int, minimum: UInt32
            var scalar: UInt32
            if c & 0xe0 == 0xc0 { count = 2; minimum = 0x80; scalar = c & 0x1f }
            else if c & 0xf0 == 0xe0 { count = 3; minimum = 0x800; scalar = c & 0x0f }
            else if c & 0xf8 == 0xf0 { count = 4; minimum = 0x10000; scalar = c & 0x07 }
            else { return false }
            if i + count > bytes.count { return false }
            for k in 1..<count {
                guard bytes[i + k] & 0xc0 == 0x80 else { return false }
                scalar = scalar << 6 | UInt32(bytes[i + k] & 0x3f)
            }
            if scalar < minimum || scalar > 0x10ffff || (0xd800...0xdfff).contains(scalar) { return false }
            if (0x80..<0xa0).contains(scalar) { return false }
            i += count
        }
        return true
    }
}
