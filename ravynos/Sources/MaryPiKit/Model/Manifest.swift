import Foundation

/// One downloadable firmware source pinned by URL, size and SHA-256.
public struct FirmwareSource: Codable, Sendable, Hashable {
    public let id: String
    public let description: String?
    public let version: String
    public let url: String
    public let sha256: String
    public let sizeBytes: Int64
    public let kind: String
    public let files: [String]

    public init(id: String, description: String? = nil, version: String, url: String, sha256: String, sizeBytes: Int64, kind: String = "zip", files: [String]) {
        self.id = id
        self.description = description
        self.version = version
        self.url = url
        self.sha256 = sha256
        self.sizeBytes = sizeBytes
        self.kind = kind
        self.files = files
    }

    public var downloadURL: URL? { URL(string: url) }
    public var archiveFileName: String { downloadURL?.lastPathComponent ?? "\(id)-\(version).\(kind)" }
}

/// The pinned set of upstream artifacts MaryPi downloads. Lives in
/// `Resources/Manifest.json`; re-pin with `scripts/update-manifest.sh`.
public struct FirmwareManifest: Codable, Sendable, Hashable {
    public let schema: Int
    public let updated: String
    public let sources: [FirmwareSource]

    public init(schema: Int = 1, updated: String, sources: [FirmwareSource]) {
        self.schema = schema
        self.updated = updated
        self.sources = sources
    }

    public static let rpi5UEFIIdentifier = "rpi5-uefi"

    public var rpi5UEFI: FirmwareSource? {
        sources.first { $0.id == Self.rpi5UEFIIdentifier }
    }

    public static func decode(_ data: Data) throws -> FirmwareManifest {
        try JSONDecoder().decode(FirmwareManifest.self, from: data)
    }

    /// The manifest compiled into MaryPiKit.
    public static func bundled() throws -> FirmwareManifest {
        guard let url = Bundle.module.url(forResource: "Manifest", withExtension: "json", subdirectory: "Resources") else {
            throw MaryPiError("Manifest.json is missing from the MaryPiKit resource bundle")
        }
        return try decode(Data(contentsOf: url))
    }

    /// Validate what a manifest must look like before anything is downloaded.
    public func validate() throws {
        guard schema == 1 else { throw MaryPiError("Unsupported manifest schema \(schema)") }
        for source in sources {
            guard source.sha256.count == 64, source.sha256.allSatisfy(\.isHexDigit) else {
                throw MaryPiError("Source \(source.id) has an invalid sha256")
            }
            guard let url = source.downloadURL, url.scheme == "https" else {
                throw MaryPiError("Source \(source.id) must use an https URL")
            }
            guard source.sizeBytes > 0 else { throw MaryPiError("Source \(source.id) has no size") }
            guard !source.files.isEmpty else { throw MaryPiError("Source \(source.id) lists no files") }
        }
        guard rpi5UEFI != nil else { throw MaryPiError("Manifest has no \(Self.rpi5UEFIIdentifier) source") }
    }
}

/// Resources compiled into MaryPiKit besides the manifest.
public enum BundledResources {
    public static func text(named name: String, extension ext: String) throws -> String {
        guard let url = Bundle.module.url(forResource: name, withExtension: ext, subdirectory: "Resources") else {
            throw MaryPiError("\(name).\(ext) is missing from the MaryPiKit resource bundle")
        }
        return try String(contentsOf: url, encoding: .utf8)
    }

    public static func ravynosConfigFragment() throws -> String {
        try text(named: "ravynos-config.fragment", extension: "txt")
    }

    public static func cardReadme() throws -> String {
        try text(named: "MARYPI-README", extension: "txt")
    }
}
