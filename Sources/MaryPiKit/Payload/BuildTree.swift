import Foundation

/// Where the ravynOS checkout and its build output live. MaryPi never builds
/// ravynOS; it only reads what the ravynOS makefiles produced.
public struct BuildTree: Sendable, Equatable {
    public enum Source: String, Sendable, Codable {
        case flag
        case environment
        case settings
        case defaultLocation
        case none

        public var description: String {
            switch self {
            case .flag: return "--ravynos"
            case .environment: return BuildTree.environmentRootKey
            case .settings: return "saved setting"
            case .defaultLocation: return "default location"
            case .none: return "not configured"
            }
        }
    }

    public static let defaultRelativeRoot = "Documents/repositories/ravynos"
    public static let environmentRootKey = "MARYPI_RAVYNOS_ROOT"
    public static let environmentBuildKey = "MARYPI_BUILD_DIR"
    public static let environmentBooterKey = "MARYPI_BOOTER"

    public let source: Source
    public let ravynosRoot: URL?
    public let buildDir: URL?
    public let booterOverride: URL?

    public init(source: Source, ravynosRoot: URL?, buildDir: URL?, booterOverride: URL? = nil) {
        self.source = source
        self.ravynosRoot = ravynosRoot
        self.buildDir = buildDir
        self.booterOverride = booterOverride
    }

    public static func isRavynOSCheckout(_ url: URL, fileManager: FileManager = .default) -> Bool {
        fileManager.fileExists(atPath: url.appending(path: "Kernel/xnu").path)
    }

    /// Resolve the checkout: flag, environment, saved setting, default path.
    public static func locate(
        explicitRoot: String? = nil,
        environment: [String: String] = ProcessInfo.processInfo.environment,
        settings: Settings = .load(),
        home: URL = FileManager.default.homeDirectoryForCurrentUser,
        fileManager: FileManager = .default
    ) -> BuildTree {
        func expand(_ path: String) -> URL {
            URL(fileURLWithPath: (path as NSString).expandingTildeInPath).standardizedFileURL
        }

        var root: URL?
        var source = Source.none
        if let explicitRoot, !explicitRoot.isEmpty {
            root = expand(explicitRoot)
            source = .flag
        } else if let fromEnvironment = environment[environmentRootKey], !fromEnvironment.isEmpty {
            root = expand(fromEnvironment)
            source = .environment
        } else if let saved = settings.ravynosRoot, !saved.isEmpty {
            root = expand(saved)
            source = .settings
        } else {
            let candidate = home.appending(path: defaultRelativeRoot).standardizedFileURL
            if isRavynOSCheckout(candidate, fileManager: fileManager) {
                root = candidate
                source = .defaultLocation
            }
        }

        var build: URL?
        if let fromEnvironment = environment[environmentBuildKey], !fromEnvironment.isEmpty {
            build = expand(fromEnvironment)
        } else if let saved = settings.buildDir, !saved.isEmpty {
            build = expand(saved)
        } else if let root {
            build = root.deletingLastPathComponent().appending(path: "build").standardizedFileURL
        }

        var booter: URL?
        if let fromEnvironment = environment[environmentBooterKey], !fromEnvironment.isEmpty {
            booter = expand(fromEnvironment)
        }

        return BuildTree(source: source, ravynosRoot: root, buildDir: build, booterOverride: booter)
    }

    public var rootIsValidCheckout: Bool {
        guard let ravynosRoot else { return false }
        return Self.isRavynOSCheckout(ravynosRoot)
    }

    /// Staging root produced by the ravynOS x86_64 build.
    public var sysrootX86: URL? { buildDir?.appending(path: "sysroot") }
    /// Staging root produced by `bmake TARGET_ARCH=arm64`.
    public var sysrootArm64: URL? { buildDir?.appending(path: "sysroot-arm64") }
    public var kernelcache: URL? { buildDir?.appending(path: "kernelcache") }
    public var kernelcacheArm64: URL? { buildDir?.appending(path: "kernelcache-arm64") }

    static let kernelFileNames = ["kernel", "kernel.development", "kernel.debug"]

    /// Places an arm64 kernel may be, most specific first.
    public var kernelCandidates: [URL] {
        guard let buildDir else { return [] }
        var candidates: [URL] = []
        for sysroot in ["sysroot-arm64", "sysroot"] {
            for name in Self.kernelFileNames {
                candidates.append(buildDir.appending(path: "\(sysroot)/System/Library/Kernels/\(name)"))
            }
        }
        candidates.append(buildDir.appending(path: "kernelcache-arm64"))
        candidates.append(buildDir.appending(path: "kernelcache"))
        return candidates
    }

    /// Places a prelinked arm64 kernelcache may be.
    public var kernelcacheCandidates: [URL] {
        [kernelcacheArm64, kernelcache].compactMap { $0 }
    }

    /// Places the AArch64 booter may be, most specific first.
    public var booterCandidates: [URL] {
        var candidates: [URL] = []
        if let booterOverride { candidates.append(booterOverride) }
        if let buildDir {
            candidates.append(buildDir.appending(path: "booter/bootaa64.efi"))
            candidates.append(buildDir.appending(path: "sysroot-arm64/System/Library/CoreServices/bootaa64.efi"))
            candidates.append(buildDir.appending(path: "sysroot/System/Library/CoreServices/bootaa64.efi"))
        }
        return candidates
    }
}
