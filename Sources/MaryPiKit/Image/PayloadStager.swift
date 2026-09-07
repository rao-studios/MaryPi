import Foundation

/// Copies files onto the mounted image volumes.
public struct PayloadStager: Sendable {
    private var fileManager: FileManager { .default }

    public init() {}

    private func replaceItem(at destination: URL, with source: URL) throws {
        if fileManager.fileExists(atPath: destination.path) {
            try fileManager.removeItem(at: destination)
        }
        try fileManager.copyItem(at: source, to: destination)
    }

    /// Populate the FAT partition: firmware, config.txt, marker, and (level >= 1)
    /// the booter, kernel and boot plist.
    public func stageFAT(volume: URL, spec: ImageSpec, log: Logger) throws {
        let firmware = spec.firmware

        let upstream = try String(contentsOf: firmware.upstreamConfigTxt, encoding: .utf8)
        let config = ConfigTxt.render(upstream: upstream, marypiVersion: MaryPi.version)
        try config.write(to: volume.appending(path: FirmwareBundle.configFileName), atomically: true, encoding: .utf8)
        log("Wrote config.txt (upstream rpi5-uefi \(firmware.version) + MaryPi block)")

        for file in firmware.verbatimFiles {
            try replaceItem(at: volume.appending(path: file.lastPathComponent), with: file)
            log("Copied \(file.lastPathComponent)")
        }
        if let overlays = firmware.overlaysDir {
            try replaceItem(at: volume.appending(path: FirmwareBundle.overlaysDirectoryName), with: overlays)
            log("Copied overlays/")
        }

        var marker = MaryPiMarker(level: spec.level, firmwareVersion: firmware.version, target: spec.targetName, ravynosGitSHA: spec.ravynosGitSHA)
        if spec.includesBoot, let kernelPath = spec.level >= .fullSystem ? spec.payload.kernelcache : spec.payload.kernel {
            marker.kernelSHA256 = try? FileHash.sha256Hex(of: URL(fileURLWithPath: kernelPath))
        }
        try marker.render().write(to: volume.appending(path: MaryPiMarker.fileName), atomically: true, encoding: .utf8)
        try BundledResources.cardReadme().write(to: volume.appending(path: "MARYPI-README.txt"), atomically: true, encoding: .utf8)
        log("Wrote \(MaryPiMarker.fileName) and MARYPI-README.txt")

        guard spec.includesBoot else { return }
        try stageBoot(volume: volume, spec: spec, log: log)
    }

    /// Booter, kernel and boot plist for payload levels 1 and 2.
    public func stageBoot(volume: URL, spec: ImageSpec, log: Logger) throws {
        guard let booterPath = spec.payload.booter else {
            throw MaryPiError("Payload level \(spec.level.rawValue) requires a booter but none was resolved")
        }
        let efiDir = volume.appending(path: "EFI/BOOT")
        try fileManager.createDirectory(at: efiDir, withIntermediateDirectories: true)
        try replaceItem(at: efiDir.appending(path: "BOOTAA64.EFI"), with: URL(fileURLWithPath: booterPath))
        log("Copied booter to EFI/BOOT/BOOTAA64.EFI")

        let ravynDir = volume.appending(path: "ravynos")
        try fileManager.createDirectory(at: ravynDir, withIntermediateDirectories: true)

        let kernelEntry: String
        if spec.level >= .fullSystem, let kernelcache = spec.payload.kernelcache {
            try replaceItem(at: ravynDir.appending(path: "kernelcache"), with: URL(fileURLWithPath: kernelcache))
            kernelEntry = BootPlist.defaultKernelcachePath
            log("Copied kernelcache to ravynos/kernelcache")
        } else if let kernel = spec.payload.kernel {
            try replaceItem(at: ravynDir.appending(path: "kernel"), with: URL(fileURLWithPath: kernel))
            kernelEntry = BootPlist.defaultKernelPath
            log("Copied kernel to ravynos/kernel")
        } else {
            throw MaryPiError("Payload level \(spec.level.rawValue) requires a kernel but none was resolved")
        }

        let plist = BootPlist(kernel: kernelEntry, kernelFlags: spec.kernelFlags)
        try plist.plistData().write(to: ravynDir.appending(path: BootPlist.fileName), options: .atomic)
        log("Wrote ravynos/\(BootPlist.fileName): Kernel=\(kernelEntry) Flags=\(spec.kernelFlags)")
    }

    /// Copy the arm64 sysroot onto the HFS+ root partition with ditto and add
    /// the few directories a Darwin root needs that the staging tree lacks.
    public func stageRoot(volume: URL, sysroot: URL, runner: CommandRunner, log: Logger) async throws {
        log("Copying \(sysroot.path) to \(volume.path) with ditto")
        try await runner.run("/usr/bin/ditto", [sysroot.path, volume.path]).checkSuccess("ditto sysroot")

        for directory in ["dev", "private/etc", "private/var/tmp", "private/tmp", "Volumes", "Users"] {
            let url = volume.appending(path: directory)
            if !fileManager.fileExists(atPath: url.path) {
                try fileManager.createDirectory(at: url, withIntermediateDirectories: true)
            }
        }
        for (link, target) in [("tmp", "private/tmp"), ("var", "private/var"), ("etc", "private/etc")] {
            let url = volume.appending(path: link)
            if !fileManager.fileExists(atPath: url.path) {
                try fileManager.createSymbolicLink(atPath: url.path, withDestinationPath: target)
            }
        }
        log("Root filesystem skeleton complete")
    }
}
