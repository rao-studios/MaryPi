import Foundation
import Virtualization

/// The files of one VM instance: a private copy of the built image, the
/// kernel files it was built with, its serial log, process id and MAC address.
public struct VMStatePaths: Sendable, Equatable {
    public let directory: URL
    public let target: ImageTarget

    public init(directory: URL, target: ImageTarget) {
        self.directory = directory
        self.target = target
    }

    public var disk: URL { directory.appending(path: "disk.img") }
    public var kernel: URL { directory.appending(path: "Image") }
    public var initrd: URL { directory.appending(path: "initrd.img") }
    public var bootInfo: URL { directory.appending(path: "boot.json") }
    public var serialLog: URL { directory.appending(path: "serial.log") }
    public var pidFile: URL { directory.appending(path: "vm.pid") }
    public var macAddressFile: URL { directory.appending(path: "mac.addr") }

    public var hasDisk: Bool { FileManager.default.fileExists(atPath: disk.path) }
}

public enum VMStateManager {
    /// Make the state directory ready to boot: an APFS clone of the built
    /// image (instant, shares blocks with the original) enlarged to
    /// `diskSize` so the guest's first boot can grow its root partition, plus
    /// the kernel, initrd and boot.json that belong to that image. Existing
    /// state is reused unless `fresh`.
    public static func prepare(_ paths: VMStatePaths, artifacts: BuildArtifacts, fresh: Bool, diskSize: Int64, log: Logger = silentLogger) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: paths.directory, withIntermediateDirectories: true)
        guard artifacts.target == .vm, let kernel = artifacts.kernel, let initrd = artifacts.initrd, let bootInfo = artifacts.bootInfo else {
            throw MaryOSError("only the vm target boots in Virtualization.framework (got \(artifacts.target.rawValue))")
        }
        let missing = artifacts.missingFiles()
        guard missing.isEmpty else {
            throw MaryOSError("no complete VM build in \(artifacts.image.deletingLastPathComponent().path); missing \(missing.map(\.lastPathComponent).joined(separator: ", ")). Build it first: maryos build --target vm")
        }
        if !fresh, paths.hasDisk {
            let diskDate = (try? fm.attributesOfItem(atPath: paths.disk.path)[.creationDate] as? Date) ?? .distantPast
            let imageDate = (try? fm.attributesOfItem(atPath: artifacts.image.path)[.modificationDate] as? Date) ?? .distantPast
            if imageDate > diskDate {
                log("Disk: reusing \(paths.disk.path), which predates the built image; --fresh boots the new image")
            } else {
                log("Disk: reusing \(paths.disk.path) (--fresh starts again from the built image)")
            }
            return
        }
        for url in [paths.disk, paths.kernel, paths.initrd, paths.bootInfo] where fm.fileExists(atPath: url.path) {
            try fm.removeItem(at: url)
        }
        try fm.copyItem(at: artifacts.image, to: paths.disk)
        try fm.copyItem(at: kernel, to: paths.kernel)
        try fm.copyItem(at: initrd, to: paths.initrd)
        try fm.copyItem(at: bootInfo, to: paths.bootInfo)
        let handle = try FileHandle(forWritingTo: paths.disk)
        defer { try? handle.close() }
        let current = try handle.seekToEnd()
        if current < UInt64(diskSize) {
            try handle.truncate(atOffset: UInt64(diskSize))
            log("Disk: fresh copy of \(artifacts.image.lastPathComponent), grown to \(ByteCountFormatter.string(fromByteCount: diskSize, countStyle: .file)) (sparse; the guest grows its root partition on first boot)")
        } else {
            log("Disk: fresh copy of \(artifacts.image.lastPathComponent) (\(ByteCountFormatter.string(fromByteCount: Int64(current), countStyle: .file)))")
        }
    }

    /// A locally administered MAC address that stays the same across runs,
    /// so the guest keeps its DHCP lease from the NAT network.
    public static func macAddress(_ paths: VMStatePaths) throws -> String {
        if let saved = try? String(contentsOf: paths.macAddressFile, encoding: .utf8) {
            let trimmed = saved.trimmingCharacters(in: .whitespacesAndNewlines)
            if VZMACAddress(string: trimmed) != nil { return trimmed }
        }
        let address = VZMACAddress.randomLocallyAdministered().string
        try FileManager.default.createDirectory(at: paths.directory, withIntermediateDirectories: true)
        try (address + "\n").write(to: paths.macAddressFile, atomically: true, encoding: .utf8)
        return address
    }

    /// Throw away the VM's disk and kernel files; the next run clones the built image again.
    public static func reset(_ paths: VMStatePaths) throws {
        let fm = FileManager.default
        for url in [paths.disk, paths.kernel, paths.initrd, paths.bootInfo] where fm.fileExists(atPath: url.path) {
            try fm.removeItem(at: url)
        }
    }
}
