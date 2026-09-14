import CryptoKit
import Foundation

/// A Noise CipherState: ChaCha20-Poly1305 whose nonce is four zero bytes and then a 64-bit counter,
/// little-endian. Without a key it passes bytes through, as the first tokens of a handshake need.
/// A failed decryption leaves the counter where it was.
public struct NoiseCipher: Sendable {
    public static let tagLength = 16

    var key: SymmetricKey?
    private(set) var nonce: UInt64 = 0

    init(key: [UInt8]? = nil) {
        self.key = key.map { SymmetricKey(data: $0) }
    }

    var hasKey: Bool { key != nil }

    static func nonceBytes(_ counter: UInt64) -> [UInt8] {
        [0, 0, 0, 0] + withUnsafeBytes(of: counter.littleEndian) { Array($0) }
    }

    mutating func encrypt(ad: [UInt8], plaintext: [UInt8]) throws -> [UInt8] {
        guard let key else { return plaintext }
        // 2^64 - 1 is reserved by the spec; nothing gets near it, but never wrap.
        guard nonce < UInt64.max else { throw MaryVNCError("the session has used every nonce") }
        let box = try ChaChaPoly.seal(plaintext, using: key, nonce: ChaChaPoly.Nonce(data: Self.nonceBytes(nonce)), authenticating: ad)
        nonce += 1
        return Array(box.ciphertext) + Array(box.tag)
    }

    mutating func decrypt(ad: [UInt8], ciphertext: [UInt8]) throws -> [UInt8] {
        guard let key else { return ciphertext }
        guard nonce < UInt64.max else { throw MaryVNCError("the session has used every nonce") }
        guard ciphertext.count >= Self.tagLength else { throw MaryVNCError("a message too short to carry its tag") }
        let plain: Data
        do {
            let box = try ChaChaPoly.SealedBox(
                nonce: ChaChaPoly.Nonce(data: Self.nonceBytes(nonce)),
                ciphertext: ciphertext.dropLast(Self.tagLength),
                tag: ciphertext.suffix(Self.tagLength)
            )
            plain = try ChaChaPoly.open(box, using: key, authenticating: ad)
        } catch {
            throw MaryVNCError("a message failed authentication")
        }
        nonce += 1
        return Array(plain)
    }

    /// One session message out, with empty associated data.
    public mutating func seal(_ plaintext: [UInt8]) throws -> [UInt8] {
        try encrypt(ad: [], plaintext: plaintext)
    }

    /// One session message in.
    public mutating func open(_ ciphertext: [UInt8]) throws -> [UInt8] {
        try decrypt(ad: [], ciphertext: ciphertext)
    }
}

/// A Noise SymmetricState over SHA-256.
struct NoiseSymmetric: Sendable {
    var cipher = NoiseCipher()
    var chainingKey: [UInt8]
    var hash: [UInt8]

    init(protocolName: String) {
        let name = Array(protocolName.utf8)
        hash = name.count <= 32 ? name + [UInt8](repeating: 0, count: 32 - name.count) : Array(SHA256.hash(data: name))
        chainingKey = hash
    }

    static func hmac(_ key: [UInt8], _ data: [UInt8]) -> [UInt8] {
        Array(HMAC<SHA256>.authenticationCode(for: data, using: SymmetricKey(data: key)))
    }

    /// The spec's HKDF, HMAC-SHA256 chained; CryptoKit's HKDF lays its blocks out differently.
    static func hkdf(_ chainingKey: [UInt8], _ input: [UInt8]) -> ([UInt8], [UInt8]) {
        let temp = hmac(chainingKey, input)
        let first = hmac(temp, [0x01])
        let second = hmac(temp, first + [0x02])
        return (first, second)
    }

    mutating func mixKey(_ input: [UInt8]) {
        let (next, key) = Self.hkdf(chainingKey, input)
        chainingKey = next
        cipher = NoiseCipher(key: key)
    }

    mutating func mixHash(_ data: [UInt8]) {
        hash = Array(SHA256.hash(data: hash + data))
    }

    mutating func encryptAndHash(_ plaintext: [UInt8]) throws -> [UInt8] {
        let ciphertext = try cipher.encrypt(ad: hash, plaintext: plaintext)
        mixHash(ciphertext)
        return ciphertext
    }

    mutating func decryptAndHash(_ ciphertext: [UInt8]) throws -> [UInt8] {
        let plaintext = try cipher.decrypt(ad: hash, ciphertext: ciphertext)
        mixHash(ciphertext)
        return plaintext
    }

    func split() -> (NoiseCipher, NoiseCipher) {
        let (first, second) = Self.hkdf(chainingKey, [])
        return (NoiseCipher(key: first), NoiseCipher(key: second))
    }
}
