import Foundation
import Observation

public struct PrepareOptions: Sendable, Equatable {
    /// Run the builder before flashing (always true when no image exists yet).
    public var build = true
    /// Rebuild the base rootfs even when the cache matches.
    public var fresh = false

    public init(build: Bool = true, fresh: Bool = false) {
        self.build = build
        self.fresh = fresh
    }
}

public enum PreparePhase: Sendable, Equatable {
    case idle
    case running
    case finished(image: URL, flashed: Bool)
    case failed(String)
}

/// Drives one complete run: doctor -> build (Docker) -> flash. Observable so
/// the SwiftUI app can render the step list and log live; the CLI prints the
/// same log.
@MainActor
@Observable
public final class PrepareCoordinator {
    public private(set) var plan = StepPlan(steps: [])
    public private(set) var logLines: [String] = []
    public private(set) var phase: PreparePhase = .idle
    public private(set) var imageURL: URL?
    public private(set) var flashProgress: FlashProgress?
    public private(set) var doctorChecks: [DoctorCheck] = []

    private let runner = CommandRunner()
    private let logSink: Logger?

    public init(logSink: Logger? = nil) {
        self.logSink = logSink
    }

    public var isRunning: Bool { phase == .running }

    private func append(_ line: String) {
        logLines.append(line)
        logSink?(line)
    }

    /// Add a line to the log (used for VM serial output in the app).
    public func log(_ line: String) {
        append(line)
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
        plan = StepPlan(steps: [])
        logLines = []
        phase = .idle
        imageURL = nil
        flashProgress = nil
    }

    private func handleBuildLine(_ line: String) {
        append("build| \(line)")
        guard let stage = BuildLog.stage(in: line) else { return }
        switch stage {
        case .rootfs:
            plan.set(.buildRootfs, .running)
        case .ui:
            plan.set(.buildRootfs, .done)
            plan.set(.buildUI, .running)
        case .target:
            plan.set(.buildRootfs, .done)
            plan.set(.buildUI, .done)
            plan.set(.buildTarget, .running)
        case .image:
            plan.set(.buildTarget, .done)
            plan.set(.buildImage, .running)
        }
    }

    /// Build an image for `target` and, when `disk` is given, write it there.
    public func prepare(target: ImageTarget, disk: DiskInfo?, paths: KitPaths, config: DistroConfig, options: PrepareOptions = PrepareOptions()) async {
        guard phase != .running else { return }
        reset()
        phase = .running
        let log = logger
        let step = stepSink
        let artifacts = BuildArtifacts.locate(target: target, config: config, paths: paths)
        let build = options.build || !artifacts.isComplete
        plan = StepPlan.standard(build: build, hasTarget: disk != nil)

        do {
            plan.set(.doctor, .running)
            let checks = await Doctor().run(paths: paths, runner: runner)
            doctorChecks = checks
            if let failure = checks.first(where: \.isBlockingFailure) {
                throw MaryOSError("This Mac is missing \(failure.name): \(failure.detail)")
            }
            plan.set(.doctor, .done)

            if build {
                append("Building \(config.imageName(for: target)) with \(paths.buildScript.path)")
                plan.set(.buildRootfs, .running)
                let builder = BuildRunner(paths: paths)
                try await builder.build(target: target, fresh: options.fresh, log: { [weak self] line in
                    Task { @MainActor in self?.handleBuildLine(line) }
                })
                plan.set(.buildRootfs, .done)
                plan.set(.buildUI, .done)
                plan.set(.buildTarget, .done)
                plan.set(.buildImage, .done, detail: artifacts.image.lastPathComponent)
            } else {
                append("Using the existing \(artifacts.image.path)")
            }
            guard artifacts.isComplete else {
                throw MaryOSError("the build finished but \(artifacts.missingFiles().map(\.lastPathComponent).joined(separator: ", ")) is missing in \(paths.outDirectory.path)")
            }
            imageURL = artifacts.image

            if let disk {
                try await Flasher(runner: runner).flash(image: artifacts.image, target: disk, log: log, progress: { [weak self] progress in
                    Task { @MainActor in
                        self?.flashProgress = progress
                        self?.plan.set(.write, .running, progress: progress.fraction)
                    }
                }, step: step)
                phase = .finished(image: artifacts.image, flashed: true)
            } else {
                phase = .finished(image: artifacts.image, flashed: false)
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
}
