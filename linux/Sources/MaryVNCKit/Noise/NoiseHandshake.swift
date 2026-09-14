import CryptoKit
import Foundation

/// A long-term or ephemeral Curve25519 key pair.
public struct NoiseKeyPair: Sendable {
    public let privateKey: [UInt8]
    public let publicKey: [UInt8]

    public init(privateKey: [UInt8]) throws {
        guard privateKey.count == 32 else { throw MaryVNCError("a Curve25519 private key is 32 bytes") }
        let key = try Curve25519.KeyAgreement.PrivateKey(rawRepresentation: privateKey)
        self.privateKey = privateKey
        self.publicKey = Array(key.publicKey.rawRepresentation)
    }

    private init(generated key: Curve25519.KeyAgreement.PrivateKey) {
        privateKey = Array(key.rawRepresentation)
        publicKey = Array(key.publicKey.rawRepresentation)
    }

    public static func generate() -> NoiseKeyPair {
        NoiseKeyPair(generated: Curve25519.KeyAgreement.PrivateKey())
    }

    /// X25519 with a remote public key; an all-zero result (a low-order point) is refused.
    func agree(with remote: [UInt8]) throws -> [UInt8] {
        guard remote.count == 32 else { throw MaryVNCError("a Curve25519 public key is 32 bytes") }
        let shared: [UInt8]
        do {
            let key = try Curve25519.KeyAgreement.PrivateKey(rawRepresentation: privateKey)
            let secret = try key.sharedSecretFromKeyAgreement(with: Curve25519.KeyAgreement.PublicKey(rawRepresentation: remote))
            shared = secret.withUnsafeBytes { Array($0) }
        } catch {
            throw MaryVNCError("a key agreement with an invalid public key")
        }
        guard shared.contains(where: { $0 != 0 }) else { throw MaryVNCError("a key agreement with a low-order point") }
        return shared
    }
}

/// The two handshakes MaryVNC uses: XX to pair (both static keys exchanged) and IK to resume (the viewer
/// already knows the Pi's key and proves its own in the first message).
public enum NoisePattern: UInt8, Sendable {
    case xx = 1
    case ik = 2

    enum Token: Sendable { case e, s, ee, es, se, ss }

    public var protocolName: String {
        switch self {
        case .xx: "Noise_XX_25519_ChaChaPoly_SHA256"
        case .ik: "Noise_IK_25519_ChaChaPoly_SHA256"
        }
    }

    var messages: [[Token]] {
        switch self {
        case .xx: [[.e], [.e, .ee, .s, .es], [.s, .se]]
        case .ik: [[.e, .es, .s, .ss], [.e, .ee, .se]]
        }
    }
}

/// A Noise HandshakeState for either role. Messages alternate, the initiator's first. After any error
/// the handshake is spent and every later call throws: a failed handshake is started again, never resumed.
public struct NoiseHandshake: Sendable {
    public let pattern: NoisePattern
    public let isInitiator: Bool
    public private(set) var remoteStatic: [UInt8]?

    private var symmetric: NoiseSymmetric
    private let staticKey: NoiseKeyPair
    private var ephemeral: NoiseKeyPair?
    private var presetEphemeral: NoiseKeyPair?
    private var remoteEphemeral: [UInt8]?
    private var index = 0
    private var spent = false

    /// `remoteStatic` is the responder's key, which IK's initiator must know. `ephemeral` fixes the next
    /// ephemeral key, for test vectors only.
    public init(pattern: NoisePattern, initiator: Bool, prologue: [UInt8] = MaryVNC.prologue, staticKey: NoiseKeyPair,
                remoteStatic: [UInt8]? = nil, ephemeral: NoiseKeyPair? = nil) throws {
        self.pattern = pattern
        self.isInitiator = initiator
        self.staticKey = staticKey
        self.presetEphemeral = ephemeral
        symmetric = NoiseSymmetric(protocolName: pattern.protocolName)
        symmetric.mixHash(prologue)
        if pattern == .ik {
            // The pre-message `<- s`: both sides hash the responder's static key.
            if initiator {
                guard let remoteStatic, remoteStatic.count == 32 else { throw MaryVNCError("IK needs the responder's static key") }
                self.remoteStatic = remoteStatic
                symmetric.mixHash(remoteStatic)
            } else {
                symmetric.mixHash(staticKey.publicKey)
            }
        }
    }

    public var isComplete: Bool { index == pattern.messages.count }
    public var isMyTurn: Bool { !spent && !isComplete && (index % 2 == 0) == isInitiator }
    public var handshakeHash: [UInt8] { symmetric.hash }

    public mutating func writeMessage(payload: [UInt8] = []) throws -> [UInt8] {
        guard isMyTurn else { throw MaryVNCError(spent ? "the handshake failed earlier" : "not this side's turn in the handshake") }
        do {
            var out: [UInt8] = []
            for token in pattern.messages[index] {
                switch token {
                case .e:
                    let key = presetEphemeral ?? NoiseKeyPair.generate()
                    presetEphemeral = nil
                    ephemeral = key
                    out += key.publicKey
                    symmetric.mixHash(key.publicKey)
                case .s:
                    out += try symmetric.encryptAndHash(staticKey.publicKey)
                default:
                    try mix(token)
                }
            }
            out += try symmetric.encryptAndHash(payload)
            index += 1
            return out
        } catch {
            spent = true
            throw error
        }
    }

    public mutating func readMessage(_ message: [UInt8]) throws -> [UInt8] {
        guard !spent, !isComplete, !isMyTurn else { throw MaryVNCError(spent ? "the handshake failed earlier" : "not the other side's turn in the handshake") }
        do {
            var at = 0
            func take(_ count: Int) throws -> [UInt8] {
                guard message.count - at >= count else { throw MaryVNCError("a handshake message too short") }
                defer { at += count }
                return Array(message[at..<at + count])
            }
            for token in pattern.messages[index] {
                switch token {
                case .e:
                    let key = try take(32)
                    remoteEphemeral = key
                    symmetric.mixHash(key)
                case .s:
                    let sealed = try take(symmetric.cipher.hasKey ? 32 + NoiseCipher.tagLength : 32)
                    remoteStatic = try symmetric.decryptAndHash(sealed)
                default:
                    try mix(token)
                }
            }
            let payload = try symmetric.decryptAndHash(Array(message[at...]))
            index += 1
            return payload
        } catch {
            spent = true
            throw error
        }
    }

    private mutating func mix(_ token: NoisePattern.Token) throws {
        func agree(_ mine: NoiseKeyPair?, _ theirs: [UInt8]?) throws -> [UInt8] {
            guard let mine, let theirs else { throw MaryVNCError("the handshake is missing a key") }
            return try mine.agree(with: theirs)
        }
        switch token {
        case .ee: symmetric.mixKey(try agree(ephemeral, remoteEphemeral))
        case .es: symmetric.mixKey(try isInitiator ? agree(ephemeral, remoteStatic) : agree(staticKey, remoteEphemeral))
        case .se: symmetric.mixKey(try isInitiator ? agree(staticKey, remoteEphemeral) : agree(ephemeral, remoteStatic))
        case .ss: symmetric.mixKey(try agree(staticKey, remoteStatic))
        case .e, .s: break
        }
    }

    /// The two session ciphers, as this side uses them: the initiator sends with the first and the
    /// responder with the second.
    public func transport() throws -> (send: NoiseCipher, receive: NoiseCipher) {
        guard isComplete, !spent else { throw MaryVNCError("the handshake is not complete") }
        let (first, second) = symmetric.split()
        return isInitiator ? (first, second) : (second, first)
    }
}
