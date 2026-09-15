import CryptoKit
import Foundation

/// MaryVNC Nearby: how the viewer finds the Pis it is paired with, and a Pi whose pairing window is open, with no
/// address typed and without a Pi showing itself to anything else on the network (MaryOS docs/14-maryvnc.md,
/// "Finding a Pi: MaryVNC Nearby"; `maryvnc/include/maryvnc/nearby.h` is the Pi's side). The viewer calls on
/// UDP 5901; a Pi answers only a call carrying the tag of a Mac it is paired with, or any call while its pairing
/// window is open. `Fixtures/nearby-vectors.json` in the tests is byte-identical to MaryOS's.
///
///     call     "MVNQ" | u8 version | challenge[16] | u8 count | count × tag[8]
///     answer   "MVNA" | u8 version | challenge[16] | u16 port | u8 flags | [pub[32] | str name] | u8 count | count × tag[8]
///     K        HMAC-SHA256(X25519(this side's static key, the other's public key), "MaryVNC/1 nearby")
///     tag      the first 8 bytes of HMAC-SHA256(K, every byte before count)
public enum Nearby {
    public static let port: UInt16 = 5901
    public static let group4 = "239.255.77.86"
    public static let group6 = "ff02::4d56:4e43"
    public static let version: UInt8 = 1
    public static let challengeLength = 16
    public static let tagLength = 8
    public static let tagsMax = 32
    public static let datagramMax = 512
    static let callMagic = Array("MVNQ".utf8)
    static let answerMagic = Array("MVNA".utf8)
    static let label = Array("MaryVNC/1 nearby".utf8)
    static let pairableFlag: UInt8 = 1

    static func key(identity: NoiseKeyPair, peer: [UInt8]) throws -> [UInt8] {
        NoiseSymmetric.hmac(try identity.agree(with: peer), label)
    }

    static func tag(key: [UInt8], over prefix: [UInt8]) -> [UInt8] {
        Array(NoiseSymmetric.hmac(key, prefix).prefix(tagLength))
    }

    static func randomBytes(_ count: Int) -> [UInt8] {
        var generator = SystemRandomNumberGenerator()
        return (0..<count).map { _ in UInt8.random(in: .min ... .max, using: &generator) }
    }

    /// Whether `tags` holds `tag`, looking at every one, so a match takes no longer to find than a miss.
    static func contains(_ tags: [[UInt8]], _ tag: [UInt8]) -> Bool {
        var hit = false
        for candidate in tags where candidate.count == tag.count {
            var difference: UInt8 = 0
            for (a, b) in zip(candidate, tag) { difference |= a ^ b }
            hit = hit || difference == 0
        }
        return hit
    }
}

/// A Pi this Mac is paired with, and the key the two share for Nearby tags.
public struct NearbyKey: Sendable {
    public let piPublicKey: [UInt8]
    let key: [UInt8]

    public init(identity: NoiseKeyPair, piPublicKey: [UInt8]) throws {
        self.piPublicKey = piPublicKey
        key = try Nearby.key(identity: identity, peer: piPublicKey)
    }
}

/// A viewer's call.
public struct NearbyCall: Equatable, Sendable {
    public var challenge: [UInt8]
    public var tags: [[UInt8]]

    public init(challenge: [UInt8], tags: [[UInt8]]) {
        self.challenge = challenge
        self.tags = tags
    }

    /// A fresh call: a random challenge, a tag for each key, random tags up to a multiple of four, shuffled, so
    /// an onlooker learns neither which Pis this Mac is paired with nor how many.
    public init(keys: [NearbyKey]) throws {
        guard keys.count <= Nearby.tagsMax else { throw MaryVNCError("a call carries tags for at most \(Nearby.tagsMax) Pis") }
        challenge = Nearby.randomBytes(Nearby.challengeLength)
        tags = []
        let prefix = self.prefix
        var tags = keys.map { Nearby.tag(key: $0.key, over: prefix) }
        while tags.count % 4 != 0 { tags.append(Nearby.randomBytes(Nearby.tagLength)) }
        var generator = SystemRandomNumberGenerator()
        tags.shuffle(using: &generator)
        self.tags = tags
    }

    public init(decoding bytes: [UInt8]) throws {
        guard bytes.count <= Nearby.datagramMax else { throw MaryVNCError("a datagram longer than Nearby allows") }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Nearby.callMagic, try reader.u8() == Nearby.version else { throw MaryVNCError("not a Nearby call") }
        challenge = try reader.take(Nearby.challengeLength)
        let count = Int(try reader.u8())
        guard count <= Nearby.tagsMax, count % 4 == 0 else { throw MaryVNCError("a call with a tag count Nearby does not allow") }
        tags = try (0..<count).map { _ in try reader.take(Nearby.tagLength) }
        try reader.finish()
    }

    /// What a tag covers: every byte before count.
    var prefix: [UInt8] { Nearby.callMagic + [Nearby.version] + challenge }

    public func encoded() throws -> [UInt8] {
        guard challenge.count == Nearby.challengeLength, tags.count <= Nearby.tagsMax, tags.count % 4 == 0,
              tags.allSatisfy({ $0.count == Nearby.tagLength }) else { throw MaryVNCError("a call Nearby does not allow") }
        var writer = ByteWriter()
        writer.raw(prefix)
        writer.u8(UInt8(tags.count))
        for tag in tags { writer.raw(tag) }
        return writer.bytes
    }

    /// The Pi's check: whether the call carries the tag for this key.
    func isTagged(for key: [UInt8]) -> Bool {
        Nearby.contains(tags, Nearby.tag(key: key, over: prefix))
    }
}

/// A Pi's answer.
public struct NearbyAnswer: Equatable, Sendable {
    /// What a Pi with its pairing window open says about itself.
    public struct Offer: Equatable, Sendable {
        public var publicKey: [UInt8]
        public var name: String

        public init(publicKey: [UInt8], name: String) {
            self.publicKey = publicKey
            self.name = name
        }
    }

    public var challenge: [UInt8]
    /// maryvncd's TCP port.
    public var port: UInt16
    public var offer: Offer?
    public var tags: [[UInt8]]

    public init(challenge: [UInt8], port: UInt16, offer: Offer?, tags: [[UInt8]]) {
        self.challenge = challenge
        self.port = port
        self.offer = offer
        self.tags = tags
    }

    public init(decoding bytes: [UInt8]) throws {
        guard bytes.count <= Nearby.datagramMax else { throw MaryVNCError("a datagram longer than Nearby allows") }
        var reader = ByteReader(bytes)
        guard try reader.take(4) == Nearby.answerMagic, try reader.u8() == Nearby.version else { throw MaryVNCError("not a Nearby answer") }
        challenge = try reader.take(Nearby.challengeLength)
        port = try reader.u16()
        let flags = try reader.u8()
        guard flags & ~Nearby.pairableFlag == 0 else { throw MaryVNCError("an answer with a flag this viewer does not know") }
        if flags & Nearby.pairableFlag != 0 {
            let publicKey = try reader.take(32)
            offer = Offer(publicKey: publicKey, name: try reader.string(max: MaryVNC.nameMax))
        } else {
            offer = nil
        }
        let count = Int(try reader.u8())
        guard count <= Nearby.tagsMax else { throw MaryVNCError("an answer with more tags than Nearby allows") }
        tags = try (0..<count).map { _ in try reader.take(Nearby.tagLength) }
        try reader.finish()
    }

    /// What a tag covers: every byte before count.
    func prefix() throws -> [UInt8] {
        guard challenge.count == Nearby.challengeLength else { throw MaryVNCError("a challenge is 16 bytes") }
        var writer = ByteWriter()
        writer.raw(Nearby.answerMagic)
        writer.u8(Nearby.version)
        writer.raw(challenge)
        writer.u16(port)
        writer.u8(offer == nil ? 0 : Nearby.pairableFlag)
        if let offer {
            guard offer.publicKey.count == 32 else { throw MaryVNCError("a Curve25519 public key is 32 bytes") }
            writer.raw(offer.publicKey)
            try writer.string(offer.name, max: MaryVNC.nameMax)
        }
        return writer.bytes
    }

    public func encoded() throws -> [UInt8] {
        guard tags.count <= Nearby.tagsMax, tags.allSatisfy({ $0.count == Nearby.tagLength }) else {
            throw MaryVNCError("an answer Nearby does not allow")
        }
        var bytes = try prefix()
        bytes.append(UInt8(tags.count))
        for tag in tags { bytes += tag }
        return bytes
    }

    /// Whether the answer carries the tag for this key: the Pi it belongs to answered.
    public func isTagged(for key: NearbyKey) -> Bool {
        guard let prefix = try? prefix() else { return false }
        return Nearby.contains(tags, Nearby.tag(key: key.key, over: prefix))
    }
}
