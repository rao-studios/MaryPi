import Foundation
import Virtualization

public struct DoctorCheck: Sendable, Identifiable, Hashable {
    public var id: String { name }
    public let name: String
    public let passed: Bool
    public let blocking: Bool
    public let detail: String

    public init(name: String, passed: Bool, blocking: Bool, detail: String) {
        self.name = name
        self.passed = passed
        self.blocking = blocking
        self.detail = detail
    }

    public var isBlockingFailure: Bool { blocking && !passed }
}

/// Checks that this Mac can do what the MaryOS kit needs: the system tools
/// the flasher uses, the kit directory, Docker for the builder, and
/// Virtualization.framework plus the entitlement for the VM.
public struct Doctor: Sendable {
    public static let requiredTools = ["/usr/sbin/diskutil", "/usr/bin/osascript", "/bin/dd", "/bin/sync", "/usr/bin/codesign", "/bin/sh"]
    public static let virtualizationEntitlement = "com.apple.security.virtualization"

    public static func dockerCandidates(environment: [String: String] = ProcessInfo.processInfo.environment,
                                        home: URL = FileManager.default.homeDirectoryForCurrentUser) -> [String] {
        var dirs = (environment["PATH"] ?? "").split(separator: ":").map(String.init)
        dirs += ["/usr/local/bin", "/opt/homebrew/bin", home.appending(path: ".docker/bin").path]
        return dirs.filter { !$0.isEmpty }.map { "\($0)/docker" }
    }

    public static func findDocker(environment: [String: String] = ProcessInfo.processInfo.environment,
                                  home: URL = FileManager.default.homeDirectoryForCurrentUser,
                                  fileManager: FileManager = .default) -> String? {
        dockerCandidates(environment: environment, home: home).first { fileManager.isExecutableFile(atPath: $0) }
    }

    private var fileManager: FileManager { .default }

    public init() {}

    /// Whether the running executable carries the virtualization entitlement.
    public static func hasVirtualizationEntitlement(executable: String = Bundle.main.executablePath ?? CommandLine.arguments[0],
                                                    runner: CommandRunner = CommandRunner()) async -> Bool {
        guard let result = try? await runner.run("/usr/bin/codesign", ["-d", "--entitlements", "-", executable]) else { return false }
        return (result.stdoutText + result.stderrText).contains(virtualizationEntitlement)
    }

    public func run(paths: KitPaths?, runner: CommandRunner = CommandRunner()) async -> [DoctorCheck] {
        var checks: [DoctorCheck] = []

        for tool in Self.requiredTools {
            let ok = fileManager.isExecutableFile(atPath: tool)
            checks.append(DoctorCheck(name: tool, passed: ok, blocking: true, detail: ok ? "present" : "missing"))
        }

        var config: DistroConfig?
        if let paths {
            do {
                let loaded = try paths.loadConfig()
                config = loaded
                checks.append(DoctorCheck(name: "kit directory", passed: true, blocking: true,
                                          detail: "\(paths.root.path) (\(loaded.prettyName), Ubuntu \(loaded.baseSuite) \(loaded.arch))"))
            } catch {
                checks.append(DoctorCheck(name: "kit directory", passed: false, blocking: true, detail: error.localizedDescription))
            }
            let outOK = (try? fileManager.createDirectory(at: paths.outDirectory, withIntermediateDirectories: true)) != nil
                && fileManager.isWritableFile(atPath: paths.outDirectory.path)
            checks.append(DoctorCheck(name: "output directory", passed: outOK, blocking: false,
                                      detail: outOK ? paths.outDirectory.path : "\(paths.outDirectory.path) is not writable"))
            let sources = paths.hasMaryUISources
            let origin = paths.maryUIIsOverride ? "\(KitPaths.maryUIEnvironmentKey) override" : "submodule maryui/"
            checks.append(DoctorCheck(name: "MaryUI sources", passed: sources, blocking: false,
                                      detail: sources ? "\(paths.maryUISource.path) (\(origin))"
                                          : "\(paths.maryUISource.path) has no Makefile (\(origin)); run `git submodule update --init` or set \(KitPaths.maryUIEnvironmentKey) to a MaryUI checkout"))
            let ui = UIArtifacts.locate(paths: paths)
            checks.append(DoctorCheck(name: "desktop build", passed: ui.isBuilt, blocking: false,
                                      detail: ui.isBuilt ? "\(ui.binary.path): \(ui.summary)" : ui.summary))
        } else {
            checks.append(DoctorCheck(name: "kit directory", passed: false, blocking: true,
                                      detail: "not found: run from the checkout (linux/), pass --kit, or set \(KitPaths.environmentKey)"))
        }

        if let docker = Self.findDocker() {
            checks.append(DoctorCheck(name: "docker", passed: true, blocking: false, detail: docker))
            let info = try? await runner.run(docker, ["info", "--format", "{{.ServerVersion}} {{.OSType}}/{{.Architecture}}, {{.NCPU}} CPUs"])
            if let info, info.succeeded {
                checks.append(DoctorCheck(name: "docker daemon", passed: true, blocking: false,
                                          detail: "running: \(info.stdoutText.trimmingCharacters(in: .whitespacesAndNewlines))"))
            } else {
                checks.append(DoctorCheck(name: "docker daemon", passed: false, blocking: false,
                                          detail: "not reachable; start Docker Desktop before building images"))
            }
        } else {
            checks.append(DoctorCheck(name: "docker", passed: false, blocking: false,
                                      detail: "not installed; image builds need Docker Desktop (docker.com)"))
        }

        let vzSupported = VZVirtualMachine.isSupported
        checks.append(DoctorCheck(name: "Virtualization.framework", passed: vzSupported, blocking: false,
                                  detail: vzSupported ? "supported on this Mac" : "not supported on this Mac; the VM cannot run here"))
        let executable = Bundle.main.executablePath ?? CommandLine.arguments[0]
        let entitled = await Self.hasVirtualizationEntitlement(executable: executable, runner: runner)
        checks.append(DoctorCheck(name: "virtualization entitlement", passed: entitled, blocking: false,
                                  detail: entitled ? "present on \(executable)" : "missing on \(executable); `maryos vm run` and the app sign themselves when needed (scripts/sign.sh does it ahead of time)"))

        if let paths, let config {
            for target in ImageTarget.allCases {
                let artifacts = BuildArtifacts.locate(target: target, config: config, paths: paths)
                if artifacts.isComplete, let size = artifacts.imageSize {
                    let when = artifacts.imageDate.map { $0.formatted(date: .abbreviated, time: .shortened) } ?? "?"
                    checks.append(DoctorCheck(name: "\(target.rawValue) image", passed: true, blocking: false,
                                              detail: "\(artifacts.image.lastPathComponent), \(ByteCountFormatter.string(fromByteCount: size, countStyle: .file)), built \(when)"))
                } else {
                    checks.append(DoctorCheck(name: "\(target.rawValue) image", passed: false, blocking: false,
                                              detail: "not built yet (maryos build --target \(target.rawValue))"))
                }
            }
        }
        return checks
    }
}
