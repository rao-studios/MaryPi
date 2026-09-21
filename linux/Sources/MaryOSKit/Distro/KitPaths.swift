import Foundation

/// Where the kit (a MaryOS checkout: `distro/`, `builder/`, `maryui/`, `mary/`) lives
/// and where its outputs go.
///
/// Lookup order for the kit directory: an explicit path, `$MARYOS_KIT_DIR`,
/// a `distro/distro.conf` found by walking up from the current directory (in each
/// directory itself, its `maryos/` or its `linux/maryos/`), the app bundle's
/// `Contents/Resources/kit`, then the MaryOS submodule of the package checkout this
/// source file belongs to (covers `swift run` from any directory).
///
/// Inside a checkout (a git work tree: `.git` is a directory in a clone and a file in
/// a submodule) images go to `out/` and VM state to `state/`; from an app bundle both
/// go to `~/Library/Caches/MaryOS`. `$MARYOS_OUT` and `$MARYOS_STATE_DIR` override.
public struct KitPaths: Sendable, Equatable {
    public static let environmentKey = "MARYOS_KIT_DIR"
    public static let outEnvironmentKey = "MARYOS_OUT"
    public static let stateEnvironmentKey = "MARYOS_STATE_DIR"

    public let root: URL
    public let isCheckout: Bool
    public let outDirectory: URL
    public let stateRoot: URL

    public init(root: URL, isCheckout: Bool, outDirectory: URL, stateRoot: URL) {
        self.root = root
        self.isCheckout = isCheckout
        self.outDirectory = outDirectory
        self.stateRoot = stateRoot
    }

    public var distroDirectory: URL { root.appending(path: "distro") }
    public var distroConfig: URL { distroDirectory.appending(path: "distro.conf") }
    public var builderDirectory: URL { root.appending(path: "builder") }
    public var buildScript: URL { builderDirectory.appending(path: "build.sh") }
    public var vmOutDirectory: URL { outDirectory.appending(path: "vm") }
    /// The design system in C, the kit's `maryui/` (libmaryui): what the desktop and the apps are drawn with.
    public var maryUISource: URL { root.appending(path: "maryui") }
    /// The desktop's C sources, the kit's `system/`: maryos-desktop, the desktop model and its services, and Mary's
    /// side of the desktop. Its Makefile builds the design system and the system apps (`apps/c/`) on the way.
    public var systemSource: URL { root.appending(path: "system") }
    /// The compiled desktop: a DESTDIR tree (`usr/bin/maryos-desktop`, `usr/share/maryos/desktop.env`) the `ui` stage writes.
    public var uiOutDirectory: URL { outDirectory.appending(path: "ui") }
    public var uiBinary: URL { uiOutDirectory.appending(path: "usr/bin/maryos-desktop") }
    /// Mary's C packages: the kit's `mary/`.
    public var marySource: URL { root.appending(path: "mary") }
    /// Mary's compiled packages: a DESTDIR tree (`usr/bin/sewnd`, `usr/share/doc/mary/`) the `mary` stage writes.
    public var maryOutDirectory: URL { outDirectory.appending(path: "mary") }
    public var maryVersionFile: URL { maryOutDirectory.appending(path: "usr/share/doc/mary/mary.env") }
    /// The application layer (the Swift package MaryFoundation, its wire), the apps built on it, and the examples a
    /// person builds on the device: the kit's `maryfoundation/`, `apps/swift/` and `apps/swift/examples/`.
    public var maryFoundationSource: URL { root.appending(path: "maryfoundation") }
    public var appsSource: URL { root.appending(path: "apps/swift") }
    public var examplesSource: URL { root.appending(path: "apps/swift/examples") }
    /// The built apps: `Applications/<Name>.app` bundles (the image's `/Applications`) and `apps.env`, which the
    /// `apps` stage writes.
    public var appsOutDirectory: URL { outDirectory.appending(path: "apps") }
    public var appsVersionFile: URL { appsOutDirectory.appending(path: "apps.env") }
    public var maryUIMakefile: URL { maryUISource.appending(path: "Makefile") }
    public var hasMaryUISources: Bool { FileManager.default.fileExists(atPath: maryUIMakefile.path) }
    public var hasSystemSources: Bool { FileManager.default.fileExists(atPath: systemSource.appending(path: "Makefile").path) }

    static func hasKit(_ url: URL, fileManager: FileManager) -> Bool {
        fileManager.fileExists(atPath: url.appending(path: "distro/distro.conf").path)
            && fileManager.fileExists(atPath: url.appending(path: "builder/build.sh").path)
    }

    public static func make(root: URL, environment: [String: String], home: URL, fileManager: FileManager = .default) -> KitPaths {
        let root = root.standardizedFileURL
        let checkout = fileManager.fileExists(atPath: root.appending(path: ".git").path)
        let caches = home.appending(path: "Library/Caches/MaryOS")
        func override(_ key: String) -> URL? {
            guard let value = environment[key], !value.isEmpty else { return nil }
            return URL(fileURLWithPath: (value as NSString).expandingTildeInPath).standardizedFileURL
        }
        let out = override(outEnvironmentKey) ?? (checkout ? root.appending(path: "out") : caches.appending(path: "out"))
        let state = override(stateEnvironmentKey) ?? (checkout ? root.appending(path: "state") : caches.appending(path: "state"))
        return KitPaths(root: root, isCheckout: checkout, outDirectory: out, stateRoot: state)
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
            candidates.append(dir.appending(path: "maryos"))
            candidates.append(dir.appending(path: "linux/maryos"))
            let parent = dir.deletingLastPathComponent()
            if parent.path == dir.path { break }
            dir = parent
        }
        if let bundleResources {
            candidates.append(bundleResources.appending(path: "kit"))
        }
        let packageRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        candidates.append(packageRoot.appending(path: "maryos"))

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
