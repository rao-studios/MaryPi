import Foundation

/// Turns a `BuildTree` into an honest `PayloadReport`.
public struct PayloadResolver: Sendable {
    private var fileManager: FileManager { .default }

    public init() {}

    private func exists(_ url: URL?) -> Bool {
        guard let url else { return false }
        return fileManager.fileExists(atPath: url.path)
    }

    public func evaluate(_ tree: BuildTree, firmwareVersion: String) -> PayloadReport {
        var report = PayloadReport(
            level: .bootstrapOnly,
            firmwareVersion: firmwareVersion,
            ravynosRoot: tree.ravynosRoot?.path,
            buildDir: tree.buildDir?.path
        )
        guard let root = tree.ravynosRoot else {
            report.findings.append(Finding(.info, "No ravynOS checkout configured. Run `marypi config set ravynos <path>` or set \(BuildTree.environmentRootKey) to enable kernel payloads."))
            return report
        }
        if !tree.rootIsValidCheckout {
            report.findings.append(Finding(.warning, "\(root.path) does not look like a ravynOS checkout (no Kernel/xnu)."))
        }
        guard let buildDir = tree.buildDir, exists(buildDir) else {
            report.findings.append(Finding(.info, "No build directory at \(tree.buildDir?.path ?? "<unset>"). Build the arm64 kernel first: bmake TARGET_ARCH=arm64 -C Kernel xnu_all."))
            return report
        }

        // Kernel
        var kernel: URL?
        var wrongArch: [(URL, Set<MachOArch>)] = []
        for candidate in tree.kernelCandidates where exists(candidate) {
            let archs = (try? MachO.architectures(at: candidate)) ?? []
            if archs.contains(where: \.isARM64) {
                kernel = candidate
                break
            }
            wrongArch.append((candidate, archs))
        }
        if let kernel {
            report.kernel = kernel.path
            report.kernelArch = "arm64"
        } else if let (url, archs) = wrongArch.first {
            let described = archs.isEmpty ? "not a Mach-O file" : archs.map(\.description).sorted().joined(separator: ", ")
            report.findings.append(Finding(.info, "Kernel at \(url.path) is \(described). Build the arm64 kernel with: bmake TARGET_ARCH=arm64 -C Kernel xnu_all."))
        } else {
            report.findings.append(Finding(.info, "No arm64 kernel found under \(buildDir.path)."))
        }

        // Booter
        let booter = tree.booterCandidates.first { exists($0) }
        if let booter {
            report.booter = booter.path
        } else {
            report.findings.append(Finding(.info, "No AArch64 booter (bootaa64.efi) found. Build it with: bmake TARGET_ARCH=arm64 -C Kernel/booter."))
        }

        guard let kernel, let booter else {
            if kernel != nil && booter == nil {
                report.findings.append(Finding(.warning, "arm64 kernel found but no booter; staying at \(PayloadLevel.bootstrapOnly.shortName)."))
            }
            if kernel == nil && booter != nil {
                report.findings.append(Finding(.warning, "Booter found but no arm64 kernel; staying at \(PayloadLevel.bootstrapOnly.shortName)."))
            }
            return report
        }
        report.level = .kernelBringUp

        // Full system: kernelcache with kexts + arm64 root filesystem
        let kernelcache = tree.kernelcacheCandidates.first { exists($0) && MachO.isARM64($0) }
        let sysroot = tree.sysrootArm64
        let launchd = sysroot?.appending(path: "sbin/launchd")
        let libSystem = sysroot?.appending(path: "usr/lib/libSystem.B.dylib")
        let storageKext = sysroot?.appending(path: "System/Library/Extensions/IOStorageFamily.kext")

        var missing: [String] = []
        if kernelcache == nil { missing.append("arm64 kernelcache (build/kernelcache-arm64)") }
        if !(launchd.map(MachO.isARM64) ?? false) { missing.append("arm64 sbin/launchd in sysroot-arm64") }
        if !(libSystem.map(MachO.isARM64) ?? false) { missing.append("arm64 usr/lib/libSystem.B.dylib in sysroot-arm64") }
        if !exists(storageKext) { missing.append("IOStorageFamily.kext in sysroot-arm64") }

        if missing.isEmpty, let kernelcache, let sysroot {
            report.level = .fullSystem
            report.kernelcache = kernelcache.path
            report.sysroot = sysroot.path
        } else {
            report.findings.append(Finding(.info, "Not a full system yet; missing: \(missing.joined(separator: "; "))."))
        }
        return report
    }
}
