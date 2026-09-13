import Foundation

/// Where the kit (`distro/`, `builder/`) lives and where its outputs go.
///
/// Lookup order for the kit directory: an explicit path, `$MARYOS_KIT_DIR`,
/// a `distro/distro.conf` found by walking up from the current directory,
/// the app bundle's `Contents/Resources/kit`, then the package checkout this
/// source file belongs to (covers `swift run` from any directory).
///
/// Inside a checkout (a `Package.swift` next to `distro/`) images go to
/// `out/` and VM state to `state/`; from an app bundle both go to
/// `~/Library/Caches/MaryOS`. `$MARYOS_OUT` and `$MARYOS_STATE_DIR` override.
///
/// The desktop's C sources (MaryUI) come from the submodule `maryui/` in the
/// kit, or from the MaryUI checkout named by `$MARYUI_DIR` (its `linux/`
/// directory), so edits there are what the builder compiles.
public struct KitPaths: Sendable, Equatable {
    public static let environmentKey = "MARYOS_KIT_DIR"
    public static let outEnvironmentKey = "MARYOS_OUT"
    public static let stateEnvironmentKey = "MARYOS_STATE_DIR"
    public static let maryUIEnvironmentKey = "MARYUI_DIR"

    public let root: URL
    public let isCheckout: Bool
    public let outDirectory: URL
    public let stateRoot: URL
    /// `<kit>/maryui/linux`, or `$MARYUI_DIR/linux` when the override is set.
    public let maryUISource: URL
    public let maryUIIsOverride: Bool

    public init(root: URL, isCheckout: Bool, outDirectory: URL, stateRoot: URL, maryUISource: URL? = nil, maryUIIsOverride: Bool = false) {
        self.root = root
        self.isCheckout = isCheckout
        self.outDirectory = outDirectory
        self.stateRoot = stateRoot
        self.maryUISource = maryUISource ?? root.appending(path: "maryui/linux")
        self.maryUIIsOverride = maryUIIsOverride
    }

    public var distroDirectory: URL { root.appending(path: "distro") }
    public var distroConfig: URL { distroDirectory.appending(path: "distro.conf") }
    public var builderDirectory: URL { root.appending(path: "builder") }
    public var buildScript: URL { builderDirectory.appending(path: "build.sh") }
    public var vmOutDirectory: URL { outDirectory.appending(path: "vm") }
    /// The compiled desktop: a DESTDIR tree (`usr/bin/maryui-desktop`, `usr/share/maryui/`) the `ui` stage writes.
    public var uiOutDirectory: URL { outDirectory.appending(path: "ui") }
    public var uiBinary: URL { uiOutDirectory.appending(path: "usr/bin/maryui-desktop") }
    /// Mary's C packages (`linux/mary`): part of this repository, never an override.
    public var marySource: URL { root.appending(path: "mary") }
    /// Mary's compiled packages: a DESTDIR tree (`usr/bin/sewnd`, `usr/share/doc/mary/`) the `mary` stage writes.
    public var maryOutDirectory: URL { outDirectory.appending(path: "mary") }
    public var maryVersionFile: URL { maryOutDirectory.appending(path: "usr/share/doc/mary/mary.env") }
    public var maryUIMakefile: URL { maryUISource.appending(path: "Makefile") }
    public var hasMaryUISources: Bool { FileManager.default.fileExists(atPath: maryUIMakefile.path) }

    static func hasKit(_ url: URL, fileManager: FileManager) -> Bool {
        fileManager.fileExists(atPath: url.appending(path: "distro/distro.conf").path)
            && fileManager.fileExists(atPath: url.appending(path: "builder/build.sh").path)
    }

    public static func make(root: URL, environment: [String: String], home: URL, fileManager: FileManager = .default) -> KitPaths {
        let root = root.standardizedFileURL
        let checkout = fileManager.fileExists(atPath: root.appending(path: "Package.swift").path)
        let caches = home.appending(path: "Library/Caches/MaryOS")
        func override(_ key: String) -> URL? {
            guard let value = environment[key], !value.isEmpty else { return nil }
            return URL(fileURLWithPath: (value as NSString).expandingTildeInPath).standardizedFileURL
        }
        let out = override(outEnvironmentKey) ?? (checkout ? root.appending(path: "out") : caches.appending(path: "out"))
        let state = override(stateEnvironmentKey) ?? (checkout ? root.appending(path: "state") : caches.appending(path: "state"))
        let maryUI = override(maryUIEnvironmentKey).map { $0.appending(path: "linux") }
        return KitPaths(root: root, isCheckout: checkout, outDirectory: out, stateRoot: state, maryUISource: maryUI, maryUIIsOverride: maryUI != nil)
    }

    public static func locate(
        explicitRoot: String? = nil,
        environment: [String: String] = ProcessInfo.processInfo.environment,
        currentDirectory: URL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath),
        bundleResources: URL? = Bundle.main.resourceURL,
        home: URL = FileManager.default.homeDirectoryForCurrentUser,
        fileManager: FileManager = .default
    ) -> KitPaths? {
        var candidates: [URL] = []
        if let explicitRoot, !explicitRoot.isEmpty {
            candidates.append(URL(fileURLWithPath: (explicitRoot as NSString).expandingTildeInPath))
        }
        if let override = environment[environmentKey], !override.isEmpty {
            candidates.append(URL(fileURLWithPath: (override as NSString).expandingTildeInPath))
        }
        var dir = currentDirectory.standardizedFileURL
        for _ in 0..<12 {
            candidates.append(dir)
            candidates.append(dir.appending(path: "linux"))
            let parent = dir.deletingLastPathComponent()
            if parent.path == dir.path { break }
            dir = parent
        }
        if let bundleResources {
            candidates.append(bundleResources.appending(path: "kit"))
        }
        let packageRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        candidates.append(packageRoot)

        guard let found = candidates.first(where: { hasKit($0, fileManager: fileManager) }) else { return nil }
        return make(root: found, environment: environment, home: home, fileManager: fileManager)
    }

    public func loadConfig() throws -> DistroConfig {
        try DistroConfig.load(from: distroConfig)
    }

    public func state(for target: ImageTarget) -> VMStatePaths {
        VMStatePaths(directory: stateRoot.appending(path: "vm/\(target.rawValue)"), target: target)
    }
}
