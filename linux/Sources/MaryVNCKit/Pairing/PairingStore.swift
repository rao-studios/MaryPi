import Foundation

/// A Pi this Mac has paired with.
public struct PairedPi: Codable, Equatable, Sendable, Identifiable {
    /// The Pi's static key, 64 hex digits in the file.
    public var publicKey: [UInt8]
    public var name: String
    public var paired: Date
    public var lastConnected: Date?

    public var fingerprint: Fingerprint { Fingerprint(publicKey: publicKey) }
    public var id: String { fingerprint.hex }

    public init(publicKey: [UInt8], name: String, paired: Date, lastConnected: Date? = nil) {
        self.publicKey = publicKey
        self.name = name
        self.paired = paired
        self.lastConnected = lastConnected
    }

    enum CodingKeys: String, CodingKey { case publicKey, name, paired, lastConnected }

    public init(from decoder: any Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let hex = try c.decode(String.self, forKey: .publicKey)
        guard hex.count == 64, let bytes = PairedPi.bytes(hex: hex) else {
            throw DecodingError.dataCorruptedError(forKey: .publicKey, in: c, debugDescription: "a public key is 64 hex digits")
        }
        publicKey = bytes
        name = try c.decode(String.self, forKey: .name)
        paired = try c.decode(Date.self, forKey: .paired)
        lastConnected = try c.decodeIfPresent(Date.self, forKey: .lastConnected)
    }

    public func encode(to encoder: any Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(publicKey.map { String(format: "%02x", $0) }.joined(), forKey: .publicKey)
        try c.encode(name, forKey: .name)
        try c.encode(paired, forKey: .paired)
        try c.encodeIfPresent(lastConnected, forKey: .lastConnected)
    }

    static func bytes(hex: String) -> [UInt8]? {
        let digits = Array(hex.utf8)
        guard digits.count % 2 == 0 else { return nil }
        var out: [UInt8] = []
        out.reserveCapacity(digits.count / 2)
        for i in stride(from: 0, to: digits.count, by: 2) {
            guard let byte = UInt8(String(decoding: digits[i..<i + 2], as: UTF8.self), radix: 16) else { return nil }
            out.append(byte)
        }
        return out
    }
}

/// The paired Pis, in `~/Library/Application Support/MaryVNC/pairs.json` (version 1, mode 0600). Keys here
/// are public; this Mac's private key is in the Keychain.
public struct PairingStore: Sendable {
    public static var defaultURL: URL {
        URL.applicationSupportDirectory.appending(path: "MaryVNC/pairs.json")
    }

    private struct File: Codable {
        var version: Int
        var pis: [PairedPi]
    }

    public let url: URL
    public private(set) var pis: [PairedPi] = []

    /// A store with nothing in it that saves to `url`, for when the file there cannot be read.
    public init(emptyAt url: URL) {
        self.url = url
    }

    /// Reads the file if there is one; a file from a later version, or one that does not parse, throws.
    public init(url: URL = PairingStore.defaultURL) throws {
        self.url = url
        guard FileManager.default.fileExists(atPath: url.path) else { return }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let file = try decoder.decode(File.self, from: Data(contentsOf: url))
        guard file.version == 1 else { throw MaryVNCError("\(url.path) is version \(file.version); this MaryVNC reads version 1") }
        pis = file.pis
    }

    public func find(_ fingerprint: Fingerprint) -> PairedPi? {
        pis.first { $0.fingerprint == fingerprint }
    }

    /// Adds a Pi or renames one already paired, keeping its history.
    public mutating func upsert(publicKey: [UInt8], name: String, now: Date = Date()) throws {
        if let index = pis.firstIndex(where: { $0.publicKey == publicKey }) {
            pis[index].name = name
        } else {
            pis.append(PairedPi(publicKey: publicKey, name: name, paired: now))
        }
        try save()
    }

    public mutating func forget(_ fingerprint: Fingerprint) throws {
        pis.removeAll { $0.fingerprint == fingerprint }
        try save()
    }

    public mutating func touch(_ fingerprint: Fingerprint, now: Date = Date()) throws {
        guard let index = pis.firstIndex(where: { $0.fingerprint == fingerprint }) else { return }
        pis[index].lastConnected = now
        try save()
    }

    /// Of the paired Pis in view, the one connected to most recently (or paired most recently, if none has
    /// connected yet): the one to connect to on its own.
    public func mostRecent(among fingerprints: Set<Fingerprint>) -> PairedPi? {
        pis.filter { fingerprints.contains($0.fingerprint) }
            .max { ($0.lastConnected ?? $0.paired) < ($1.lastConnected ?? $1.paired) }
    }

    private func save() throws {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(File(version: 1, pis: pis))
        let directory = url.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        try data.write(to: url, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
    }
}
