import Foundation

/// One of the builder's stages, as announced on its log (`==> stage: ...`).
public enum BuildStage: Sendable, Equatable {
    case rootfs
    /// The desktop (MaryUI/linux) compiled into `out/ui`.
    case ui
    /// Mary's packages (linux/mary) compiled into `out/mary`.
    case mary
    case target(ImageTarget)
    case image(ImageTarget)
}

public enum BuildLog {
    /// The builder colours its stage lines on a terminal; remove `ESC[...m`.
    public static func stripANSI(_ line: String) -> String {
        line.replacing(/\u{1B}\[[0-9;]*m/, with: "")
    }

    /// `==> stage: target_build vm` -> `.target(.vm)`; nil for any other line.
    public static func stage(in line: String) -> BuildStage? {
        let plain = stripANSI(line)
        guard let range = plain.range(of: "==> stage: ") else { return nil }
        let words = plain[range.upperBound...].split(whereSeparator: \.isWhitespace).map(String.init)
        guard let name = words.first else { return nil }
        let target = words.dropFirst().first.flatMap(ImageTarget.init(rawValue:))
        switch name {
        case "rootfs_build": return .rootfs
        case "ui_build": return .ui
        case "mary_build": return .mary
        case "target_build": return target.map(BuildStage.target)
        case "image_build": return target.map(BuildStage.image)
        default: return nil
        }
    }
}

/// Runs `builder/build.sh`, which builds inside Docker on a Mac.
public struct BuildRunner: Sendable {
    /// Directories Docker Desktop and Homebrew put their binaries in, added
    /// to PATH because an app launched from Finder has almost none.
    public static let searchPath = "/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"

    public let paths: KitPaths

    public init(paths: KitPaths) {
        self.paths = paths
    }

    public func environment(home: URL = FileManager.default.homeDirectoryForCurrentUser) -> [String: String] {
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = ((env["PATH"] ?? "") + ":" + Self.searchPath + ":" + home.appending(path: ".docker/bin").path)
            .split(separator: ":").filter { !$0.isEmpty }.map(String.init).joined(separator: ":")
        env[KitPaths.outEnvironmentKey] = paths.outDirectory.path
        return env
    }

    /// Exit status of `build.sh <arguments>`, with every log line delivered as it appears.
    public func run(_ arguments: [String], log: @escaping Logger) async throws -> Int32 {
        try await StreamingProcess.run(
            executable: "/bin/sh",
            arguments: [paths.buildScript.path] + arguments,
            environment: environment(),
            currentDirectory: paths.root,
            onLine: log
        )
    }

    /// Stages that take no target: they run once for every image.
    public static let targetlessStages = ["rootfs", "ui", "mary"]

    /// `build.sh all <target>` (or a single stage); throws when the builder fails.
    public func build(target: ImageTarget, stage: String = "all", fresh: Bool = false, keep: Bool = false, dryRun: Bool = false, log: @escaping Logger) async throws {
        var arguments = [stage]
        if !Self.targetlessStages.contains(stage) { arguments.append(target.rawValue) }
        if fresh { arguments.append("--fresh") }
        if keep { arguments.append("--keep") }
        if dryRun { arguments.append("--dry-run") }
        let status = try await run(arguments, log: log)
        guard status == 0 else {
            throw MaryOSError("build.sh \(arguments.joined(separator: " ")) failed with exit status \(status); see the log above")
        }
    }
}

/// The boot files the builder records for the VM target (`out/vm/boot.json`).
public struct VMBootInfo: Codable, Sendable, Equatable {
    public var kernelVersion: String
    public var kernel: String
    public var initrd: String
    public var cmdline: String
    public var built: String

    public init(kernelVersion: String, kernel: String, initrd: String, cmdline: String, built: String) {
        self.kernelVersion = kernelVersion
        self.kernel = kernel
        self.initrd = initrd
        self.cmdline = cmdline
        self.built = built
    }

    public static func load(from url: URL) throws -> VMBootInfo {
        do {
            return try JSONDecoder().decode(VMBootInfo.self, from: Data(contentsOf: url))
        } catch {
            throw MaryOSError("cannot read \(url.path): \(error.localizedDescription)")
        }
    }
}

/// What the builder's `ui` stage leaves in `out/ui`: the compiled desktop as a
/// DESTDIR tree with PREFIX=/usr, plus `usr/share/maryui/maryui.env` naming the
/// MaryUI commit it came from.
public struct UIArtifacts: Sendable, Equatable {
    public let directory: URL
    public let binary: URL
    public let versionFile: URL
    public let renders: URL

    public static func locate(paths: KitPaths) -> UIArtifacts {
        UIArtifacts(directory: paths.uiOutDirectory, binary: paths.uiBinary,
                    versionFile: paths.uiOutDirectory.appending(path: "usr/share/maryui/maryui.env"),
                    renders: paths.uiOutDirectory.appending(path: "renders"))
    }

    public init(directory: URL, binary: URL, versionFile: URL, renders: URL) {
        self.directory = directory
        self.binary = binary
        self.versionFile = versionFile
        self.renders = renders
    }

    public var isBuilt: Bool { FileManager.default.isExecutableFile(atPath: binary.path) }

    /// `MARYUI_GIT_SHA`, `SOURCE`, `BUILT` from maryui.env.
    public var info: [String: String] {
        guard let text = try? String(contentsOf: versionFile, encoding: .utf8) else { return [:] }
        return (try? ConfParser.parse(text)) ?? [:]
    }

    public var gitSHA: String? { info["MARYUI_GIT_SHA"] }
    public var built: String? { info["BUILT"] }

    public var binaryDate: Date? {
        try? FileManager.default.attributesOfItem(atPath: binary.path)[.modificationDate] as? Date
    }

    /// `maryui abc123def456, built 2026-09-08T16:00:00Z` or `not built yet`.
    public var summary: String {
        guard isBuilt else { return "not built yet (maryos build --stage ui)" }
        return "maryui \(gitSHA ?? "unknown"), built \(built ?? "?")"
    }
}

/// What the builder's `mary` stage leaves in `out/mary`: Mary's packages as a
/// DESTDIR tree with PREFIX=/usr (`usr/bin/sewnd`, `threadd`, `maryd`, …), plus
/// `usr/share/doc/mary/mary.env` naming the MaryPi commit they came from.
public struct MaryArtifacts: Sendable, Equatable {
    public let directory: URL
    public let binaries: URL
    public let versionFile: URL

    public static func locate(paths: KitPaths) -> MaryArtifacts {
        MaryArtifacts(directory: paths.maryOutDirectory, binaries: paths.maryOutDirectory.appending(path: "usr/bin"),
                      versionFile: paths.maryVersionFile)
    }

    public init(directory: URL, binaries: URL, versionFile: URL) {
        self.directory = directory
        self.binaries = binaries
        self.versionFile = versionFile
    }

    /// The stage writes mary.env last, so its presence means the tree is whole.
    public var isBuilt: Bool { FileManager.default.fileExists(atPath: versionFile.path) }

    /// `MARY_GIT_SHA`, `SOURCE`, `BUILT` from mary.env.
    public var info: [String: String] {
        guard let text = try? String(contentsOf: versionFile, encoding: .utf8) else { return [:] }
        return (try? ConfParser.parse(text)) ?? [:]
    }

    public var gitSHA: String? { info["MARY_GIT_SHA"] }
    public var built: String? { info["BUILT"] }

    /// The programs the stage installed, by name.
    public var programs: [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: binaries.path)) ?? []).sorted()
    }

    /// `mary abc123def456, built 2026-09-12T20:00:00Z: maryctl maryd sewnd` or `not built yet`.
    public var summary: String {
        guard isBuilt else { return "not built yet (maryos build --stage mary)" }
        let list = programs
        return "mary \(gitSHA ?? "unknown"), built \(built ?? "?")" + (list.isEmpty ? "" : ": \(list.joined(separator: " "))")
    }
}

/// What the builder leaves in `out/` for one target, whether or not it exists yet.
public struct BuildArtifacts: Sendable, Equatable {
    public let target: ImageTarget
    public let image: URL
    public let manifest: URL
    public let checksum: URL
    public let kernel: URL?
    public let initrd: URL?
    public let bootInfo: URL?

    public static func locate(target: ImageTarget, config: DistroConfig, paths: KitPaths) -> BuildArtifacts {
        let name = config.imageName(for: target)
        let image = paths.outDirectory.appending(path: name)
        let vm = paths.vmOutDirectory
        return BuildArtifacts(
            target: target,
            image: image,
            manifest: paths.outDirectory.appending(path: name + ".txt"),
            checksum: paths.outDirectory.appending(path: name + ".sha256"),
            kernel: target == .vm ? vm.appending(path: "Image") : nil,
            initrd: target == .vm ? vm.appending(path: "initrd.img") : nil,
            bootInfo: target == .vm ? vm.appending(path: "boot.json") : nil
        )
    }

    public init(target: ImageTarget, image: URL, manifest: URL, checksum: URL, kernel: URL?, initrd: URL?, bootInfo: URL?) {
        self.target = target
        self.image = image
        self.manifest = manifest
        self.checksum = checksum
        self.kernel = kernel
        self.initrd = initrd
        self.bootInfo = bootInfo
    }

    public var requiredFiles: [URL] {
        [image] + [kernel, initrd, bootInfo].compactMap { $0 }
    }

    public func missingFiles(fileManager: FileManager = .default) -> [URL] {
        requiredFiles.filter { !fileManager.fileExists(atPath: $0.path) }
    }

    public var imageExists: Bool { FileManager.default.fileExists(atPath: image.path) }
    public var isComplete: Bool { missingFiles().isEmpty }

    public var imageSize: Int64? {
        (try? FileManager.default.attributesOfItem(atPath: image.path)[.size] as? NSNumber)?.int64Value
    }

    public var imageDate: Date? {
        try? FileManager.default.attributesOfItem(atPath: image.path)[.modificationDate] as? Date
    }

    /// The MARYOS.txt the builder wrote next to the image, if any.
    public var manifestText: String? {
        try? String(contentsOf: manifest, encoding: .utf8)
    }
}
