import Foundation

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

/// Checks that this Mac can do what MaryPi needs.
public struct Doctor: Sendable {
    public static let requiredTools = [
        "/usr/sbin/diskutil", "/usr/bin/hdiutil", "/usr/bin/osascript", "/bin/dd",
        "/usr/bin/ditto", "/usr/bin/du", "/bin/sync",
    ]
    public static let optionalTools = [
        "/opt/homebrew/bin/qemu-system-aarch64", "/opt/homebrew/bin/dtc", "/opt/homebrew/bin/bmake",
    ]

    private var fileManager: FileManager { .default }

    public init() {}

    public func run(tree: BuildTree, manifest: FirmwareManifest?, cache: FirmwareCache?) async -> [DoctorCheck] {
        var checks: [DoctorCheck] = []

        for tool in Self.requiredTools {
            let ok = fileManager.isExecutableFile(atPath: tool)
            checks.append(DoctorCheck(name: tool, passed: ok, blocking: true, detail: ok ? "present" : "missing"))
        }
        for tool in Self.optionalTools {
            let ok = fileManager.isExecutableFile(atPath: tool)
            checks.append(DoctorCheck(name: tool, passed: ok, blocking: false, detail: ok ? "present" : "not installed (optional; brew install qemu dtc bmake)"))
        }

        if let root = tree.ravynosRoot {
            let valid = tree.rootIsValidCheckout
            checks.append(DoctorCheck(name: "ravynOS checkout", passed: valid, blocking: false,
                                      detail: "\(root.path) via \(tree.source.description)\(valid ? "" : " (no Kernel/xnu found)")"))
        } else {
            checks.append(DoctorCheck(name: "ravynOS checkout", passed: false, blocking: false,
                                      detail: "not configured; marypi config set ravynos <path> (Level 0 still works)"))
        }
        if let build = tree.buildDir {
            let exists = fileManager.fileExists(atPath: build.path)
            checks.append(DoctorCheck(name: "ravynOS build directory", passed: exists, blocking: false,
                                      detail: exists ? build.path : "\(build.path) does not exist yet"))
        }
        let kernel = tree.kernelCandidates.first { fileManager.fileExists(atPath: $0.path) && MachO.isARM64($0) }
        checks.append(DoctorCheck(name: "arm64 kernel", passed: kernel != nil, blocking: false,
                                  detail: kernel?.path ?? "none (bmake TARGET_ARCH=arm64 -C Kernel xnu_all)"))
        let booter = tree.booterCandidates.first { fileManager.fileExists(atPath: $0.path) }
        checks.append(DoctorCheck(name: "AArch64 booter", passed: booter != nil, blocking: false,
                                  detail: booter?.path ?? "none (bmake TARGET_ARCH=arm64 -C Kernel/booter)"))

        if let manifest {
            do {
                try manifest.validate()
                checks.append(DoctorCheck(name: "firmware manifest", passed: true, blocking: true,
                                          detail: "schema \(manifest.schema), updated \(manifest.updated)"))
            } catch {
                checks.append(DoctorCheck(name: "firmware manifest", passed: false, blocking: true, detail: error.localizedDescription))
            }
            if let cache, let source = manifest.rpi5UEFI {
                let ready = await cache.isReady(source)
                let status = await cache.status(source)
                checks.append(DoctorCheck(name: "firmware cache", passed: ready, blocking: false, detail: status))
            }
        } else {
            checks.append(DoctorCheck(name: "firmware manifest", passed: false, blocking: true, detail: "could not load Manifest.json"))
        }

        return checks
    }
}
