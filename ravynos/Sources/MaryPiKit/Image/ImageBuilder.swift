import Foundation

/// Builds a raw MBR disk image entirely without root:
/// sparse file -> hdiutil attach -nomount -> diskutil partitionDisk -> copy -> eject.
public struct ImageBuilder: Sendable {
    public static let hdiutil = "/usr/bin/hdiutil"
    public static let du = "/usr/bin/du"

    private let runner: CommandRunner
    private let diskUtil: DiskUtil
    private let stager: PayloadStager
    private var fileManager: FileManager { .default }

    public init(runner: CommandRunner = CommandRunner()) {
        self.runner = runner
        self.diskUtil = DiskUtil(runner: runner)
        self.stager = PayloadStager()
    }

    public typealias StepSink = @Sendable (StepKind, StepStatus) -> Void

    public func build(_ spec: ImageSpec, log: Logger = silentLogger, step: @escaping StepSink = { _, _ in }) async throws -> URL {
        // 1. Sparse image file
        step(.createImage, .running)
        var sysrootBytes: Int64 = 0
        if spec.includesRoot, let sysroot = spec.payload.sysroot {
            sysrootBytes = try await directorySize(URL(fileURLWithPath: sysroot))
        }
        let size = spec.imageSizeBytes(sysrootBytes: sysrootBytes)
        try fileManager.createDirectory(at: spec.outputURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        if fileManager.fileExists(atPath: spec.outputURL.path) {
            try fileManager.removeItem(at: spec.outputURL)
        }
        guard fileManager.createFile(atPath: spec.outputURL.path, contents: nil) else {
            throw MaryPiError("Cannot create \(spec.outputURL.path)")
        }
        let handle = try FileHandle(forWritingTo: spec.outputURL)
        try handle.truncate(atOffset: UInt64(size))
        try handle.close()
        log("Created \(spec.outputURL.path) (\(ByteCountFormatter.string(fromByteCount: size, countStyle: .file)))")
        step(.createImage, .done)

        // 2. Attach without mounting
        step(.partition, .running)
        let disk = try await attach(spec.outputURL)
        log("Attached as /dev/\(disk)")

        do {
            // 3. Partition
            try await diskUtil.partitionMBR(disk, partitions: [
                ("MS-DOS FAT32", ImageSpec.fatLabel, "\(spec.firmwarePartitionMiB)M"),
                ("Journaled HFS+", ImageSpec.rootLabel, "R"),
            ])
            let fatVolume = try await mountPoint(of: "\(disk)s1")
            let rootVolume = try await mountPoint(of: "\(disk)s2")
            log("Partitioned: \(disk)s1 FAT32 at \(fatVolume.path), \(disk)s2 HFS+ at \(rootVolume.path)")
            step(.partition, .done)

            // 4. Firmware + boot files
            step(.populateFirmware, .running)
            try stager.stageFAT(volume: fatVolume, spec: spec, log: log)
            step(.populateFirmware, .done)
            if spec.includesBoot {
                step(.populateBoot, .done)
            }

            // 5. Root filesystem
            if spec.includesRoot, let sysroot = spec.payload.sysroot {
                step(.populateRoot, .running)
                try await stager.stageRoot(volume: rootVolume, sysroot: URL(fileURLWithPath: sysroot), runner: runner, log: log)
                step(.populateRoot, .done)
            } else if spec.includesMinimalRoot, let initProgram = spec.payload.initProgram {
                step(.populateRoot, .running)
                try stager.stageMinimalRoot(volume: rootVolume, initProgram: URL(fileURLWithPath: initProgram), log: log)
                step(.populateRoot, .done)
            }

            // 6. Detach
            step(.ejectImage, .running)
            try await detach(disk, log: log)
            step(.ejectImage, .done)
        } catch {
            try? await detach(disk, log: log)
            throw error
        }

        log("Image ready: \(spec.outputURL.path)")
        return spec.outputURL
    }

    /// Attach a raw image with no partition map and return its `diskN`.
    func attach(_ image: URL) async throws -> String {
        let result = try await runner.run(Self.hdiutil, [
            "attach", "-nomount", "-imagekey", "diskimage-class=CRawDiskImage", image.path,
        ]).checkSuccess("hdiutil attach")
        guard let disk = Self.parseAttachedDisk(result.stdoutText) else {
            throw MaryPiError("Could not find the attached disk in hdiutil output: \(result.stdoutText)")
        }
        return disk
    }

    /// `/dev/disk6 <tab> ... ` -> `disk6`
    public static func parseAttachedDisk(_ output: String) -> String? {
        for line in output.split(whereSeparator: \.isNewline) {
            guard let token = line.split(whereSeparator: \.isWhitespace).first else { continue }
            let name = DiskUtil.normalize(String(token))
            if DiskUtil.isValidDiskIdentifier(name) { return name }
        }
        return nil
    }

    func mountPoint(of partition: String) async throws -> URL {
        for attempt in 0..<10 {
            if let path = try await diskUtil.mountPoint(of: partition), !path.isEmpty {
                return URL(fileURLWithPath: path)
            }
            if attempt == 2 {
                try? await diskUtil.mount(partition)
            }
            try await Task.sleep(for: .milliseconds(300))
        }
        throw MaryPiError("\(partition) did not mount")
    }

    func detach(_ disk: String, log: Logger) async throws {
        do {
            try await diskUtil.eject(disk)
            log("Detached /dev/\(disk)")
        } catch {
            log("diskutil eject failed (\(error.localizedDescription)); forcing hdiutil detach")
            try await runner.run(Self.hdiutil, ["detach", "/dev/\(disk)", "-force"]).checkSuccess("hdiutil detach")
        }
    }

    func directorySize(_ url: URL) async throws -> Int64 {
        let result = try await runner.run(Self.du, ["-sk", url.path]).checkSuccess("du -sk")
        let field = result.stdoutText.split(whereSeparator: \.isWhitespace).first ?? ""
        guard let kilobytes = Int64(field) else {
            throw MaryPiError("Unexpected du output: \(result.stdoutText)")
        }
        return kilobytes * 1024
    }
}
