import Foundation

/// Where the committed `vm/` directory and the per-profile state live.
///
/// Lookup order for `vm/`: `$MARYPI_VM_DIR`, a `vm/profiles` directory found
/// by walking up from the current directory, the app bundle's
/// `Contents/Resources/vm`, then the package checkout this source file
/// belongs to (covers `swift run` from any directory).
///
/// State goes to `<vm>/state/` inside a checkout (the directory next to
/// `Package.swift`), otherwise to the user's cache directory; `$MARYPI_VM_STATE`
/// overrides.
public struct VMPaths: Sendable, Equatable {
    public static let environmentDirKey = "MARYPI_VM_DIR"
    public static let environmentStateKey = "MARYPI_VM_STATE"

    public let vmDirectory: URL
    public let stateRoot: URL

    public init(vmDirectory: URL, stateRoot: URL) {
        self.vmDirectory = vmDirectory
        self.stateRoot = stateRoot
    }

    public static func defaultStateRoot(for vmDirectory: URL, environment: [String: String], home: URL, fileManager: FileManager = .default) -> URL {
        if let override = environment[environmentStateKey], !override.isEmpty {
            return URL(fileURLWithPath: (override as NSString).expandingTildeInPath).standardizedFileURL
        }
        if fileManager.fileExists(atPath: vmDirectory.deletingLastPathComponent().appending(path: "Package.swift").path) {
            return vmDirectory.appending(path: "state")
        }
        #if os(macOS)
        return home.appending(path: "Library/Caches/MaryPi/vm")
        #else
        if let xdg = environment["XDG_CACHE_HOME"], !xdg.isEmpty {
            return URL(fileURLWithPath: xdg).appending(path: "marypi/vm")
        }
        return home.appending(path: ".cache/marypi/vm")
        #endif
    }

    public static func locate(
        environment: [String: String] = ProcessInfo.processInfo.environment,
        currentDirectory: URL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath),
        bundleResources: URL? = Bundle.main.resourceURL,
        home: URL = FileManager.default.homeDirectoryForCurrentUser,
        fileManager: FileManager = .default
    ) -> VMPaths? {
        func hasProfiles(_ url: URL) -> Bool {
            fileManager.fileExists(atPath: url.appending(path: "profiles").path)
        }
        var candidates: [URL] = []
        if let override = environment[environmentDirKey], !override.isEmpty {
            candidates.append(URL(fileURLWithPath: (override as NSString).expandingTildeInPath))
        }
        var dir = currentDirectory.standardizedFileURL
        for _ in 0..<12 {
            candidates.append(dir.appending(path: "vm"))
            let parent = dir.deletingLastPathComponent()
            if parent.path == dir.path { break }
            dir = parent
        }
        if let bundleResources {
            candidates.append(bundleResources.appending(path: "vm"))
        }
        let packageRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        candidates.append(packageRoot.appending(path: "vm"))

        guard let found = candidates.first(where: hasProfiles) else { return nil }
        let vm = found.standardizedFileURL
        return VMPaths(vmDirectory: vm, stateRoot: defaultStateRoot(for: vm, environment: environment, home: home, fileManager: fileManager))
    }

    public var profilesDirectory: URL { vmDirectory.appending(path: "profiles") }
    public var runScript: URL { vmDirectory.appending(path: "run.sh") }

    public func profileURL(_ name: String) -> URL {
        profilesDirectory.appending(path: "\(name).conf")
    }

    public func profileNames(fileManager: FileManager = .default) -> [String] {
        let names = (try? fileManager.contentsOfDirectory(atPath: profilesDirectory.path)) ?? []
        return names.filter { $0.hasSuffix(".conf") }.map { String($0.dropLast(5)) }.sorted()
    }

    public func loadProfile(_ name: String) throws -> VMProfile {
        let url = profileURL(name)
        guard FileManager.default.fileExists(atPath: url.path) else {
            throw MaryPiError("no VM profile named \(name) in \(profilesDirectory.path) (available: \(profileNames().joined(separator: ", ")))")
        }
        return try VMProfile.load(from: url)
    }

    public func state(for profile: String, environment: [String: String] = ProcessInfo.processInfo.environment) -> VMStatePaths {
        VMStatePaths(directory: stateRoot.appending(path: profile), profile: profile, environment: environment)
    }
}

/// The files of one running (or stopped) VM instance.
public struct VMStatePaths: Sendable, Equatable {
    /// Unix socket paths are limited to about 100 bytes on both platforms.
    public static let maximumSocketPathLength = 100

    public let directory: URL
    public let profile: String
    public let qmpSocketPath: String

    public init(directory: URL, profile: String, environment: [String: String] = ProcessInfo.processInfo.environment) {
        self.directory = directory
        self.profile = profile
        let preferred = directory.appending(path: "qmp.sock").path
        if preferred.utf8.count > Self.maximumSocketPathLength {
            let tmp = environment["TMPDIR"].flatMap { $0.isEmpty ? nil : $0 } ?? "/tmp"
            qmpSocketPath = "\(tmp)/marypi-vm-\(profile).sock"
        } else {
            qmpSocketPath = preferred
        }
    }

    public var varsFlash: URL { directory.appending(path: "vars.fd") }
    public var paddedCode: URL { directory.appending(path: "code.fd") }
    public var disk: URL { directory.appending(path: "disk.img") }
    public var serialLog: URL { directory.appending(path: "serial.log") }
    public var qemuOutput: URL { directory.appending(path: "qemu.out") }
    public var pidFile: URL { directory.appending(path: "qemu.pid") }
    public var qmpPathFile: URL { directory.appending(path: "qmp.path") }
    public var usesFallbackSocket: Bool { qmpSocketPath != directory.appending(path: "qmp.sock").path }
}
