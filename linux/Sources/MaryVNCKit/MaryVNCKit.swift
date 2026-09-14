import Foundation
import os

/// MaryVNC's viewer side: the Noise handshake, the wire, discovery and the session that talk to
/// `maryvncd` on a Pi (MaryOS `maryvnc/`, docs/14-maryvnc.md). Every layout here follows
/// `maryvnc/include/maryvnc/wire.h` byte for byte; the two sides share one Noise vector fixture.
public enum MaryVNC {
    public static let version: UInt16 = 1
    public static let serviceType = "_maryvnc._tcp"
    public static let port: UInt16 = 5901
    /// The Noise prologue both sides mix in before the first message.
    public static let prologue = Array("MaryVNC/1".utf8)
    /// The first record's magic, before the pattern byte and Noise message 1.
    public static let magic = Array("MVNC".utf8)
    /// A display name in a handshake payload or `hello`.
    public static let nameMax = 64
    /// A `bye` reason.
    public static let reasonMax = 32
    /// A handshake record's body.
    public static let handshakeMax = 1024
    /// A session message's plaintext: 4 MiB, a deliberate step past Noise's 65,535 bytes so a whole
    /// frame's JPEGs fit in one message.
    public static let messageMax = 4 << 20
    /// A session record's body: the message and its tag.
    public static let recordMax = messageMax + NoiseCipher.tagLength
    public static let rectsMax = 8

    static let logger = Logger(subsystem: "com.maryos.MaryVNC", category: "kit")
}

public struct MaryVNCError: Error, Equatable, Sendable, CustomStringConvertible {
    public let description: String

    public init(_ description: String) {
        self.description = description
    }
}
