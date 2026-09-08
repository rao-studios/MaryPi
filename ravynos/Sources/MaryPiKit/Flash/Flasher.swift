import Foundation

public struct FlashProgress: Sendable, Equatable {
    public let bytesWritten: Int64
    public let totalBytes: Int64

    public init(bytesWritten: Int64, totalBytes: Int64) {
        self.bytesWritten = bytesWritten
        self.totalBytes = totalBytes
    }

    public var fraction: Double {
        totalBytes > 0 ? min(1, Double(bytesWritten) / Double(totalBytes)) : 0
    }
}

/// Writes an image to a removable disk with one admin prompt.
public struct Flasher: Sendable {
    public typealias ProgressSink = @Sendable (FlashProgress) -> Void
    public typealias StepSink = @Sendable (StepKind, StepStatus) -> Void

    private let runner: CommandRunner
    private let diskUtil: DiskUtil
    private let privileged: PrivilegedRunner
    private var fileManager: FileManager { .default }

    public init(runner: CommandRunner = CommandRunner()) {
        self.runner = runner
        self.diskUtil = DiskUtil(runner: runner)
        self.privileged = PrivilegedRunner(runner: runner)
    }

    /// Parse dd's SIGINFO output: `517996544 bytes transferred in 12.3 secs`.
    public static func bytesTransferred(in line: String) -> Int64? {
        guard let match = line.firstMatch(of: /^\s*([0-9]+) bytes transferred/) else { return nil }
        return Int64(match.1)
    }

    /// Re-read the disk right before writing and refuse anything surprising.
    public func verify(target: DiskInfo, imageSize: Int64) async throws -> DiskInfo {
        guard DiskUtil.isValidDiskIdentifier(target.bsdName) else {
            throw MaryPiError("\(target.bsdName) is not a whole-disk identifier")
        }
        let fresh = try await diskUtil.info(target.bsdName)
        if let reason = DiskFilter.rejectionReason(fresh) {
            throw MaryPiError("Refusing to write to \(fresh.bsdName) (\(fresh.displayName)): \(reason)")
        }
        guard fresh.totalSize == target.totalSize else {
            throw MaryPiError("\(fresh.bsdName) changed size since it was selected (\(fresh.totalSize) vs \(target.totalSize)); not writing")
        }
        guard imageSize <= fresh.totalSize else {
            throw MaryPiError("Image (\(ByteCountFormatter.string(fromByteCount: imageSize, countStyle: .file))) is larger than \(fresh.bsdName) (\(fresh.sizeDescription))")
        }
        return fresh
    }

    public func flash(
        image: URL,
        target: DiskInfo,
        log: @escaping Logger = silentLogger,
        progress: @escaping ProgressSink = { _ in },
        step: @escaping StepSink = { _, _ in }
    ) async throws {
        step(.verifyTarget, .running)
        let attributes = try fileManager.attributesOfItem(atPath: image.path)
        let imageSize = (attributes[.size] as? NSNumber)?.int64Value ?? 0
        let fresh = try await verify(target: target, imageSize: imageSize)
        log("Target verified: /dev/\(fresh.bsdName) \(fresh.displayName) (\(fresh.sizeDescription), \(fresh.busProtocol ?? "unknown bus"))")
        step(.verifyTarget, .done)

        let workDir = fileManager.temporaryDirectory.appending(path: "MaryPi/\(UUID().uuidString)")
        try fileManager.createDirectory(at: workDir, withIntermediateDirectories: true)
        let scriptURL = workDir.appending(path: "flash.sh")
        let logURL = workDir.appending(path: "flash.log")
        try FlashScript.render(image: image, disk: fresh.bsdName, log: logURL).write(to: scriptURL, atomically: true, encoding: .utf8)
        try Data().write(to: logURL)
        log("Privileged script: \(scriptURL.path)")
        log("Privileged log: \(logURL.path)")

        let tailer = LogTailer(url: logURL)
        let consumer = Task {
            var sawEnd = false
            for await line in tailer.lines {
                log("  | \(line)")
                if line.hasPrefix(FlashScript.stepMarker) {
                    let name = line.dropFirst(FlashScript.stepMarker.count).trimmingCharacters(in: .whitespaces)
                    switch name {
                    case "unmount":
                        step(.unmount, .running)
                    case "write":
                        step(.unmount, .done)
                        step(.write, .running)
                    case "eject":
                        step(.write, .done)
                        progress(FlashProgress(bytesWritten: imageSize, totalBytes: imageSize))
                        step(.eject, .running)
                    default:
                        break
                    }
                } else if let bytes = Flasher.bytesTransferred(in: line) {
                    progress(FlashProgress(bytesWritten: bytes, totalBytes: imageSize))
                } else if line.hasPrefix(FlashScript.endMarker) {
                    sawEnd = true
                }
            }
            return sawEnd
        }
        tailer.start()

        let result = try await privileged.run(script: scriptURL)
        tailer.stop()
        let sawEnd = await consumer.value

        guard result.succeeded, sawEnd else {
            let scriptLog = (try? String(contentsOf: logURL, encoding: .utf8)) ?? ""
            let errorLine = scriptLog.split(whereSeparator: \.isNewline).last { $0.hasPrefix(FlashScript.errorMarker) }
                .map { String($0.dropFirst(FlashScript.errorMarker.count)).trimmingCharacters(in: .whitespaces) }
            let stderr = result.stderrText.trimmingCharacters(in: .whitespacesAndNewlines)
            let detail = errorLine ?? (stderr.isEmpty ? "see \(logURL.path)" : stderr)
            step(.write, .failed(detail))
            throw MaryPiError("Writing the card failed: \(detail)")
        }
        step(.eject, .done)
        log("Card written and ejected. Insert it into the Raspberry Pi 5.")
    }
}
