import Foundation
import Observation

public struct PrepareOptions: Sendable, Equatable {
    public var qemuVirt = false
    public var outputURL: URL?
    public var kernelFlags: String?
    public var forceFirmwareFetch = false

    public init(qemuVirt: Bool = false, outputURL: URL? = nil, kernelFlags: String? = nil, forceFirmwareFetch: Bool = false) {
        self.qemuVirt = qemuVirt
        self.outputURL = outputURL
        self.kernelFlags = kernelFlags
        self.forceFirmwareFetch = forceFirmwareFetch
    }
}

public enum PreparePhase: Sendable, Equatable {
    case idle
    case running
    case finished(image: URL, flashed: Bool)
    case failed(String)
}

/// Drives one complete run: doctor -> payload -> firmware -> image -> flash.
/// Observable so the SwiftUI app can render the step list and log live.
@MainActor
@Observable
public final class PrepareCoordinator {
    public private(set) var plan = FlashPlan(steps: [])
    public private(set) var logLines: [String] = []
    public private(set) var phase: PreparePhase = .idle
    public private(set) var imageURL: URL?
    public private(set) var flashProgress: FlashProgress?
    public private(set) var payload: PayloadReport?
    public private(set) var doctorChecks: [DoctorCheck] = []

    private let runner = CommandRunner()
    private let firmwareCache: FirmwareCache
    private let logSink: Logger?

    public init(firmwareCache: FirmwareCache = FirmwareCache(), logSink: Logger? = nil) {
        self.firmwareCache = firmwareCache
        self.logSink = logSink
    }

    public var isRunning: Bool { phase == .running }

    private func append(_ line: String) {
        logLines.append(line)
        logSink?(line)
    }

    private var logger: Logger {
        { [weak self] line in
            Task { @MainActor in self?.append(line) }
        }
    }

    private var stepSink: @Sendable (StepKind, StepStatus) -> Void {
        { [weak self] kind, status in
            Task { @MainActor in self?.plan.set(kind, status) }
        }
    }

    public func reset() {
        plan = FlashPlan(steps: [])
        logLines = []
        phase = .idle
        imageURL = nil
        flashProgress = nil
    }

    /// Build an image and, when `target` is given, write it to that disk.
    public func prepare(target: DiskInfo?, tree: BuildTree, options: PrepareOptions = PrepareOptions()) async {
        guard phase != .running else { return }
        reset()
        phase = .running
        let log = logger
        let step = stepSink

        do {
            // Manifest + doctor
            let manifest = try FirmwareManifest.bundled()
            try manifest.validate()
            guard let source = manifest.rpi5UEFI else { throw MaryPiError("Manifest has no rpi5-uefi source") }

            plan = FlashPlan.standard(level: .bootstrapOnly, hasTarget: target != nil)
            plan.set(.doctor, .running)
            let checks = await Doctor().run(tree: tree, manifest: manifest, cache: firmwareCache)
            doctorChecks = checks
            if let failure = checks.first(where: \.isBlockingFailure) {
                throw MaryPiError("This Mac is missing \(failure.name): \(failure.detail)")
            }
            plan.set(.doctor, .done)

            // Payload
            plan.set(.resolvePayload, .running)
            let report = PayloadResolver().evaluate(tree, firmwareVersion: source.version)
            payload = report
            plan = FlashPlan.standard(level: report.level, hasTarget: target != nil)
            plan.set(.doctor, .done)
            plan.set(.resolvePayload, .done, detail: report.level.title)
            append("Payload: \(report.level.shortName) (\(report.level.title))")
            for finding in report.findings { append("  \(finding.severity.rawValue): \(finding.message)") }

            // Firmware
            plan.set(.fetchFirmware, .running)
            let bundle = try await firmwareCache.ensure(source, force: options.forceFirmwareFetch, log: log) { fraction in
                Task { @MainActor [weak self] in self?.plan.set(.fetchFirmware, .running, progress: fraction) }
            }
            plan.set(.fetchFirmware, .done, detail: "rpi5-uefi \(bundle.version)")

            // Image
            let imagesDirectory = await firmwareCache.imagesDirectory()
            let output = options.outputURL
                ?? ImageSpec.defaultOutputURL(level: report.level, qemuVirt: options.qemuVirt, in: imagesDirectory)
            let spec = ImageSpec(
                payload: report,
                firmware: bundle,
                outputURL: output,
                kernelFlags: options.kernelFlags,
                qemuVirt: options.qemuVirt,
                ravynosGitSHA: await Self.gitSHA(of: tree.ravynosRoot, runner: runner)
            )
            let image = try await ImageBuilder(runner: runner).build(spec, log: log, step: step)
            imageURL = image

            // Flash
            if let target {
                try await Flasher(runner: runner).flash(image: image, target: target, log: log, progress: { [weak self] progress in
                    Task { @MainActor in
                        self?.flashProgress = progress
                        self?.plan.set(.write, .running, progress: progress.fraction)
                    }
                }, step: step)
                phase = .finished(image: image, flashed: true)
            } else {
                phase = .finished(image: image, flashed: false)
            }
        } catch {
            let message = error.localizedDescription
            append("Error: \(message)")
            if let running = plan.steps.first(where: { $0.status == .running }) {
                plan.set(running.kind, .failed(message))
            }
            phase = .failed(message)
        }
    }

    static func gitSHA(of root: URL?, runner: CommandRunner) async -> String? {
        guard let root else { return nil }
        guard let result = try? await runner.run("/usr/bin/git", ["-C", root.path, "rev-parse", "--short=12", "HEAD"]), result.succeeded else {
            return nil
        }
        return result.stdoutText.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
