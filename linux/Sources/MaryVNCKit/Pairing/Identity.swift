import CryptoKit
import Foundation
import Security
import Synchronization

/// A key's fingerprint: the SHA-256 of its raw public key, as `maryvncd` and `maryvncctl` print it.
public struct Fingerprint: Hashable, Sendable, CustomStringConvertible {
    /// 64 hex digits.
    public let hex: String

    public init(publicKey: [UInt8]) {
        hex = SHA256.hash(data: publicKey).map { String(format: "%02x", $0) }.joined()
    }

    /// Accepts 64 hex digits, as the TXT record's `id` carries them.
    public init?(hex: String) {
        let lower = hex.lowercased()
        guard lower.count == 64, lower.allSatisfy({ $0.isHexDigit }) else { return nil }
        self.hex = lower
    }

    /// The first 16 bytes in eight groups of four, as the Pi shows it.
    public var display: String { groups(8) }
    /// Two groups, for a sidebar row.
    public var short: String { groups(2) }
    public var description: String { display }

    private func groups(_ count: Int) -> String {
        let digits = Array(hex)
        return (0..<count).map { String(digits[$0 * 4..<$0 * 4 + 4]) }.joined(separator: " ")
    }
}

/// Where this Mac's private key lives.
public protocol IdentityStore: Sendable {
    func load() throws -> [UInt8]?
    func save(_ privateKey: [UInt8]) throws
    func delete() throws
}

/// The login Keychain: a generic password (service `com.maryos.MaryVNC`), readable while the Mac is
/// unlocked and never synced.
public struct KeychainIdentityStore: IdentityStore {
    public let service: String
    public let account: String

    public init(service: String = "com.maryos.MaryVNC", account: String = "identity") {
        self.service = service
        self.account = account
    }

    private var query: [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service, kSecAttrAccount as String: account]
    }

    public func load() throws -> [UInt8]? {
        var q = query
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        let status = SecItemCopyMatching(q as CFDictionary, &item)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = item as? Data else { throw MaryVNCError("the Keychain would not give MaryVNC its key (\(status))") }
        return Array(data)
    }

    public func save(_ privateKey: [UInt8]) throws {
        try delete()
        var q = query
        q[kSecValueData as String] = Data(privateKey)
        q[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
        q[kSecAttrLabel as String] = "MaryVNC identity"
        let status = SecItemAdd(q as CFDictionary, nil)
        guard status == errSecSuccess else { throw MaryVNCError("the Keychain would not keep MaryVNC's key (\(status))") }
    }

    public func delete() throws {
        let status = SecItemDelete(query as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else { throw MaryVNCError("the Keychain would not remove MaryVNC's key (\(status))") }
    }
}

/// A key in a file of its own (mode 0600), for tests and scripted runs that must not touch the Keychain.
public struct FileIdentityStore: IdentityStore {
    public let url: URL

    public init(url: URL) {
        self.url = url
    }

    public func load() throws -> [UInt8]? {
        guard FileManager.default.fileExists(atPath: url.path) else { return nil }
        let data = try Data(contentsOf: url)
        guard data.count == 32 else { throw MaryVNCError("\(url.path) is not a 32-byte key") }
        return Array(data)
    }

    public func save(_ privateKey: [UInt8]) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try delete()
        // Made 0600 from the start, never readable by others even for a moment.
        guard FileManager.default.createFile(atPath: url.path, contents: Data(privateKey), attributes: [.posixPermissions: 0o600]) else {
            throw MaryVNCError("could not write \(url.path)")
        }
    }

    public func delete() throws {
        if FileManager.default.fileExists(atPath: url.path) { try FileManager.default.removeItem(at: url) }
    }
}

/// For tests and previews.
public final class InMemoryIdentityStore: IdentityStore {
    private let key = Mutex<[UInt8]?>(nil)

    public init() {}

    public func load() throws -> [UInt8]? { key.withLock { $0 } }
    public func save(_ privateKey: [UInt8]) throws { key.withLock { $0 = privateKey } }
    public func delete() throws { key.withLock { $0 = nil } }
}

public enum Identity {
    /// This Mac's key pair, made and stored the first time.
    public static func loadOrCreate(from store: some IdentityStore) throws -> NoiseKeyPair {
        if let privateKey = try store.load() {
            return try NoiseKeyPair(privateKey: privateKey)
        }
        let pair = NoiseKeyPair.generate()
        try store.save(pair.privateKey)
        return pair
    }

    /// The name the Pi files this Mac under: the computer's name, without control characters, cut to
    /// the wire's 64 bytes on a character boundary.
    public static func wireName(_ name: String) -> String {
        var out = ""
        for character in name where !character.unicodeScalars.contains(where: { $0.value < 0x20 || (0x7f..<0xa0).contains($0.value) }) {
            guard out.utf8.count + String(character).utf8.count <= MaryVNC.nameMax else { break }
            out.append(character)
        }
        return out.isEmpty ? "Mac" : out
    }

    public static var displayName: String {
        wireName(Host.current().localizedName ?? "Mac")
    }
}
