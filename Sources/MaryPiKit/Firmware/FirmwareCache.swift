import Foundation

/// Downloads, verifies and extracts pinned firmware into
/// `~/Library/Caches/MaryPi/<source-id>/<version>/`.
public actor FirmwareCache {
    public static let verifiedStamp = ".verified"

    public let rootDirectory: URL
    private let fileManager = FileManager.default

    public static func defaultDirectory(home: URL = FileManager.default.homeDirectoryForCurrentUser) -> URL {
        home.appending(path: "Library/Caches/MaryPi")
    }

    public init(rootDirectory: URL? = nil) {
        self.rootDirectory = rootDirectory ?? Self.defaultDirectory()
    }

    public func directory(for source: FirmwareSource) -> URL {
        rootDirectory.appending(path: "\(source.id)/\(source.version)")
    }

    public func imagesDirectory() -> URL {
        rootDirectory.appending(path: "images")
    }

    /// True when the source was previously downloaded, verified and extracted.
    public func isReady(_ source: FirmwareSource) -> Bool {
        let dir = directory(for: source)
        guard let stamp = try? String(contentsOf: dir.appending(path: Self.verifiedStamp), encoding: .utf8),
              stamp.trimmingCharacters(in: .whitespacesAndNewlines) == source.sha256 else {
            return false
        }
        return (try? FirmwareBundle(source: source, directory: dir)) != nil
    }

    public func bundleIfReady(_ source: FirmwareSource) -> FirmwareBundle? {
        guard isReady(source) else { return nil }
        return try? FirmwareBundle(source: source, directory: directory(for: source))
    }

    /// Human-readable cache state for `doctor`.
    public func status(_ source: FirmwareSource) -> String {
        isReady(source)
            ? "\(source.id) \(source.version) verified at \(directory(for: source).path)"
            : "\(source.id) \(source.version) not downloaded (will be fetched into \(directory(for: source).path))"
    }

    /// Ensure the source is present and verified, downloading if needed.
    public func ensure(
        _ source: FirmwareSource,
        force: Bool = false,
        log: Logger = silentLogger,
        progress: @escaping @Sendable (Double) -> Void = { _ in }
    ) async throws -> FirmwareBundle {
        let dir = directory(for: source)
        if !force, let bundle = bundleIfReady(source) {
            log("Firmware \(source.id) \(source.version) already verified in \(dir.path)")
            progress(1)
            return bundle
        }

        guard let url = source.downloadURL else {
            throw MaryPiError("Invalid download URL for \(source.id): \(source.url)")
        }
        if fileManager.fileExists(atPath: dir.path) {
            try fileManager.removeItem(at: dir)
        }
        try fileManager.createDirectory(at: dir, withIntermediateDirectories: true)

        log("Downloading \(url.absoluteString)")
        progress(0.05)
        let (temporary, response) = try await URLSession.shared.download(from: url)
        if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            throw MaryPiError("Download of \(url.absoluteString) failed with HTTP \(http.statusCode)")
        }
        let archive = dir.appending(path: source.archiveFileName)
        if fileManager.fileExists(atPath: archive.path) {
            try fileManager.removeItem(at: archive)
        }
        try fileManager.moveItem(at: temporary, to: archive)
        progress(0.6)

        let size = (try fileManager.attributesOfItem(atPath: archive.path)[.size] as? NSNumber)?.int64Value ?? -1
        guard size == source.sizeBytes else {
            throw MaryPiError("Downloaded \(source.archiveFileName) is \(size) bytes, manifest expects \(source.sizeBytes)")
        }
        let digest = try FileHash.sha256Hex(of: archive)
        guard digest == source.sha256.lowercased() else {
            throw MaryPiError("SHA-256 mismatch for \(source.archiveFileName): got \(digest), manifest expects \(source.sha256)")
        }
        log("Verified \(source.archiveFileName): \(source.sizeBytes) bytes, sha256 \(digest)")
        progress(0.7)

        switch source.kind {
        case "zip":
            let runner = CommandRunner()
            try await runner.run("/usr/bin/ditto", ["-x", "-k", archive.path, dir.path]).checkSuccess("ditto -x -k")
        default:
            throw MaryPiError("Unsupported archive kind \(source.kind) for \(source.id)")
        }
        progress(0.9)

        let bundle = try FirmwareBundle(source: source, directory: dir)
        try (source.sha256 + "\n").write(to: dir.appending(path: Self.verifiedStamp), atomically: true, encoding: .utf8)
        log("Extracted \(source.files.count) files into \(dir.path)")
        progress(1)
        return bundle
    }
}
