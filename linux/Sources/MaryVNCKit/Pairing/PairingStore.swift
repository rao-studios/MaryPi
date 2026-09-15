import Foundation

/// A Pi this Mac has paired with.
public struct PairedPi: Codable, Equatable, Sendable, Identifiable {
    /// The Pi's static key, 64 hex digits in the file.
    public var publicKey: [UInt8]
    public var name: String
    public var paired: Date
    public var lastConnected: Date?
    /// Where the Pi last answered or was reached: the first place a Nearby call goes, which also finds a Pi on a
    /// network that drops multicast.
    public var lastAddress: String?

    public var fingerprint: Fingerprint { Fingerprint(publicKey: publicKey) }
    public var id: String { fingerprint.hex }

    public init(publicKey: [UInt8], name: String, paired: Date, lastConnected: Date? = nil, lastAddress: String? = nil) {
        self.publicKey = publicKey
        self.name = name
        self.paired = paired
        self.lastConnected = lastConnected
        self.lastAddress = lastAddress
    }

    enum CodingKeys: String, CodingKey { case publicKey, name, paired, lastConnected, lastAddress }

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
        lastAddress = try c.decodeIfPresent(String.self, forKey: .lastAddress)
    }

    public func encode(to encoder: any Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(publicKey.map { String(format: "%02x", $0) }.joined(), forKey: .publicKey)
        try c.encode(name, forKey: .name)
        try c.encode(paired, forKey: .paired)
        try c.encodeIfPresent(lastConnected, forKey: .lastConnected)
        try c.encodeIfPresent(lastAddress, forKey: .lastAddress)
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
/// are public; this Mac's private key is in the Keychain. MaryVNC and MaryVNC Light share the file, so every change
/// reads it again under a lock (`pairs.lock` beside it) before writing, and neither viewer undoes the other's.
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
    /// The file's modification date when this store last read or wrote it.
    private var modified: Date?

    /// A store with nothing in it that saves to `url`, for when the file there cannot be read.
    public init(emptyAt url: URL) {
        self.url = url
    }

    /// Reads the file if there is one; a file from a later version, or one that does not parse, throws.
    public init(url: URL = PairingStore.defaultURL) throws {
        self.url = url
        (pis, modified) = try Self.read(url)
    }

    /// Reads the file again when it changed since this store last read or wrote it: the other viewer paired or
    /// forgot a Pi. Returns whether it did.
    @discardableResult
    public mutating func reloadIfChanged() -> Bool {
        guard Self.modificationDate(url) != modified, let fresh = try? Self.read(url) else { return false }
        (pis, modified) = fresh
        return true
    }

    public func find(_ fingerprint: Fingerprint) -> PairedPi? {
        pis.first { $0.fingerprint == fingerprint }
    }

    /// Adds a Pi or renames one already paired, keeping its history.
    public mutating func upsert(publicKey: [UInt8], name: String, now: Date = Date()) throws {
        try change { pis in
            if let index = pis.firstIndex(where: { $0.publicKey == publicKey }) {
                pis[index].name = name
            } else {
                pis.append(PairedPi(publicKey: publicKey, name: name, paired: now))
            }
            return true
        }
    }

    public mutating func forget(_ fingerprint: Fingerprint) throws {
        try change { pis in
            let count = pis.count
            pis.removeAll { $0.fingerprint == fingerprint }
            return pis.count != count
        }
    }

    /// A session reached the Pi, at `address` when it is known.
    public mutating func touch(_ fingerprint: Fingerprint, address: String? = nil, now: Date = Date()) throws {
        try change { pis in
            guard let index = pis.firstIndex(where: { $0.fingerprint == fingerprint }) else { return false }
            pis[index].lastConnected = now
            if let address { pis[index].lastAddress = address }
            return true
        }
    }

    /// The Pi answered a Nearby call from `address`, with its tag.
    public mutating func remember(address: String, for fingerprint: Fingerprint) throws {
        guard find(fingerprint).map({ $0.lastAddress != address }) == true else { return }
        try change { pis in
            guard let index = pis.firstIndex(where: { $0.fingerprint == fingerprint }), pis[index].lastAddress != address else { return false }
            pis[index].lastAddress = address
            return true
        }
    }

    /// Of the paired Pis in view, the one connected to most recently (or paired most recently, if none has
    /// connected yet): the one to connect to on its own.
    public func mostRecent(among fingerprints: Set<Fingerprint>) -> PairedPi? {
        pis.filter { fingerprints.contains($0.fingerprint) }
            .max { ($0.lastConnected ?? $0.paired) < ($1.lastConnected ?? $1.paired) }
    }

    /// Under the lock: the file read again, `body` applied, and the file written when `body` says it changed
    /// something. A file that cannot be read keeps what this store holds, which the write then puts in its place.
    private mutating func change(_ body: (inout [PairedPi]) -> Bool) throws {
        let directory = url.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let lockPath = directory.appending(path: "pairs.lock").path
        let lock = open(lockPath, O_CREAT | O_RDWR | O_CLOEXEC, 0o600)
        guard lock >= 0 else { throw MaryVNCError("could not open \(lockPath): \(String(cString: strerror(errno)))") }
        defer { close(lock) }
        guard flock(lock, LOCK_EX) == 0 else { throw MaryVNCError("could not lock \(lockPath): \(String(cString: strerror(errno)))") }
        if let fresh = try? Self.read(url) { (pis, modified) = fresh }
        guard body(&pis) else { return }
        try save()
    }

    private mutating func save() throws {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(File(version: 1, pis: pis))
        try data.write(to: url, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
        modified = Self.modificationDate(url)
    }

    private static func read(_ url: URL) throws -> (pis: [PairedPi], modified: Date?) {
        guard FileManager.default.fileExists(atPath: url.path) else { return ([], nil) }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let file = try decoder.decode(File.self, from: Data(contentsOf: url))
        guard file.version == 1 else { throw MaryVNCError("\(url.path) is version \(file.version); this MaryVNC reads version 1") }
        return (file.pis, modificationDate(url))
    }

    private static func modificationDate(_ url: URL) -> Date? {
        (try? FileManager.default.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date
    }
}
