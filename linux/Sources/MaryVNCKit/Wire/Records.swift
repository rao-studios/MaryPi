import Foundation

/// Records: a u32 length, little-endian, then the body. The viewer's first record is `"MVNC"`, the pattern
/// byte and Noise message 1; later handshake records carry a Noise message alone; after the handshake each
/// body is one sealed `WireMessage`.
public enum Record {
    public static func framed(_ body: [UInt8], maximum: Int = MaryVNC.recordMax) throws -> [UInt8] {
        guard !body.isEmpty else { throw MaryVNCError("an empty record") }
        guard body.count <= maximum else { throw MaryVNCError("a record past its limit") }
        var w = ByteWriter()
        w.u32(UInt32(body.count))
        w.raw(body)
        return w.bytes
    }

    /// The whole first record, length included.
    public static func first(pattern: NoisePattern, noise: [UInt8]) throws -> [UInt8] {
        guard noise.count <= MaryVNC.handshakeMax else { throw MaryVNCError("a handshake message past its limit") }
        return try framed(MaryVNC.magic + [pattern.rawValue] + noise)
    }

    /// A first record's body back into its pattern and Noise message.
    public static func parseFirst(_ body: [UInt8]) throws -> (pattern: NoisePattern, noise: [UInt8]) {
        guard body.count >= 5, body.count - 5 <= MaryVNC.handshakeMax, Array(body.prefix(4)) == MaryVNC.magic else {
            throw MaryVNCError("not a MaryVNC first record")
        }
        guard let pattern = NoisePattern(rawValue: body[4]) else { throw MaryVNCError("a handshake pattern this viewer does not know") }
        return (pattern, Array(body.dropFirst(5)))
    }
}

/// Reassembles records from a byte stream that arrives in pieces.
public struct RecordReader: Sendable {
    public var maximum: Int
    private var buffer: [UInt8] = []
    private var start = 0

    public init(maximum: Int = MaryVNC.recordMax) {
        self.maximum = maximum
    }

    public var bufferedCount: Int { buffer.count - start }

    public mutating func append(_ bytes: some Sequence<UInt8>) {
        if start > 0, start >= buffer.count / 2 {
            buffer.removeFirst(start)
            start = 0
        }
        buffer.append(contentsOf: bytes)
    }

    /// The next whole record's body, nil while more bytes are needed. An empty record or one past
    /// `maximum` throws, and the stream is then unusable.
    public mutating func next() throws -> [UInt8]? {
        guard bufferedCount >= 4 else { return nil }
        let length = Int(UInt32(buffer[start]) | UInt32(buffer[start + 1]) << 8 | UInt32(buffer[start + 2]) << 16 | UInt32(buffer[start + 3]) << 24)
        guard length > 0 else { throw MaryVNCError("an empty record") }
        guard length <= maximum else { throw MaryVNCError("a record past its limit") }
        guard bufferedCount - 4 >= length else { return nil }
        let body = Array(buffer[(start + 4)..<(start + 4 + length)])
        start += 4 + length
        return body
    }
}
