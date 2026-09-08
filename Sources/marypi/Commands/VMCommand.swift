import ArgumentParser
import Foundation
import MaryPiKit

extension VMAccel: ExpressibleByArgument {}
extension VMDisplayMode: ExpressibleByArgument {}

struct VMCommand: AsyncParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "vm",
        abstract: "Boot a MaryPi image in QEMU (macOS hvf / Linux kvm / tcg) in a window or on the terminal.",
        subcommands: [Run.self, Stop.self, Status.self, Serial.self, Doctor.self],
        defaultSubcommand: Status.self
    )

    struct ProfileOption: ParsableArguments {
        @Option(name: .long, help: "VM profile from vm/profiles (qemu-virt, qemu-virt-gicv3).")
        var profile = "qemu-virt"

        func paths() throws -> VMPaths {
            guard let paths = VMPaths.locate() else {
                throw ValidationError("vm/ directory not found: run from the MaryPi checkout or set \(VMPaths.environmentDirKey)")
            }
            return paths
        }
    }

    struct Run: AsyncParsableCommand {
        static let configuration = CommandConfiguration(commandName: "run", abstract: "Build (macOS) or take an image and boot it.")

        @OptionGroup var global: GlobalOptions
        @OptionGroup var profileOption: ProfileOption

        @Option(name: .long, help: "Existing image to boot. Without it, a --qemu image is built into the VM state (macOS only).")
        var image: String?

        @Option(name: .long, help: "hvf | kvm | tcg (default: best the profile allows).")
        var accel: VMAccel?

        @Option(name: .long, help: "window | serial | none (default window).")
        var display: VMDisplayMode = .window

        @Option(name: .long, help: "Guest memory in MiB (default from the profile).")
        var memory: Int?

        @Option(name: .long, help: "Kernel flags for the image built here (default: -v serial=3 debug=0x8 cpus=1, plus rd=disk0s2 with a storage stack).")
        var kernelFlags: String?

        @Flag(name: .long, help: "Print the QEMU command line and exit.")
        var dryRun = false

        @Flag(name: .long, help: "Leave QEMU running after this command returns.")
        var detach = false

        @Flag(name: .long, help: "Do not echo the serial log on this terminal.")
        var quiet = false

        @Argument(parsing: .postTerminator, help: "Extra QEMU arguments after --.")
        var qemuArguments: [String] = []

        func run() async throws {
            let paths = try profileOption.paths()
            let profile = try paths.loadProfile(profileOption.profile)
            let host = await VMHost.detect(probe: .live())
            guard let qemu = host.qemu else {
                throw ValidationError("qemu-system-aarch64 not found (brew install qemu / apt install qemu-system-arm)")
            }
            guard let firmware = host.firmware else {
                throw ValidationError("no EDK2 AArch64 firmware found; set \(VMHost.firmwareCodeEnvironmentKey)")
            }
            let state = paths.state(for: profile.name)

            let imageURL: URL
            if let image {
                imageURL = URL(fileURLWithPath: (image as NSString).expandingTildeInPath).standardizedFileURL
                guard dryRun || FileManager.default.fileExists(atPath: imageURL.path) else {
                    throw ValidationError("image \(imageURL.path) does not exist")
                }
            } else if dryRun {
                imageURL = state.disk
            } else {
                #if os(macOS)
                try FileManager.default.createDirectory(at: state.directory, withIntermediateDirectories: true)
                let coordinator = await PrepareCoordinator(logSink: Output.stdoutLogger)
                await coordinator.prepare(target: nil, tree: global.tree(), options: PrepareOptions(qemuVirt: true, outputURL: state.disk, kernelFlags: kernelFlags))
                guard case .finished(let built, _) = await coordinator.phase else {
                    if case .failed(let message) = await coordinator.phase { throw ValidationError(message) }
                    throw ExitCode.failure
                }
                imageURL = built
                #else
                throw ValidationError("building images needs macOS; pass --image with an image built there")
                #endif
            }

            let resolvedAccel = try host.resolveAccel(requested: accel, profile: profile)
            let (mode, backend) = host.displayBackend(for: display)
            if display == .window && mode != .window {
                Output.line("vm: no window backend available; running without a display (serial.log only)")
            }
            // QEMU is a child of this process, which is usually not the terminal's
            // foreground process group (swift run), so it must never touch the tty.
            let spec = QEMULaunchSpec(profile: profile, image: imageURL, firmware: firmware, state: state,
                                      accel: resolvedAccel, cpu: host.cpu(for: resolvedAccel, profile: profile),
                                      display: mode, displayBackend: backend, memoryMiB: memory,
                                      serialOnTerminal: false, extraArguments: qemuArguments)
            let command = QEMUCommand(qemu: qemu, spec: spec)
            if dryRun {
                Output.line(command.commandLine)
                return
            }

            try VMController.prepareState(state, firmware: firmware, image: imageURL, accel: resolvedAccel, backend: backend, log: Output.stdoutLogger)
            let session = await VMSession(paths: state)
            var echoSerial: Logger? = nil
            if !quiet && !detach {
                echoSerial = { line in
                    print(line)
                    fflush(stdout)
                }
            }
            try await session.launch(command, detach: detach, serial: echoSerial, log: { line in
                if line.hasPrefix("QEMU running") { print("vm: \(line)") }
            })
            Output.line("vm: \(resolvedAccel.rawValue), -cpu \(spec.cpu), display \(backend.rawValue); serial log \(state.serialLog.path)")
            if mode == .serial {
                Output.line("vm: serial output follows (Ctrl-C stops the VM); for an interactive console use vm/run.sh run --display serial")
            }
            if detach {
                Output.line("vm: running in the background (pid \(await session.pid ?? 0)); marypi vm status|serial|stop --profile \(profile.name)")
                return
            }
            let sigint = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
            signal(SIGINT, SIG_IGN)
            sigint.setEventHandler {
                Task { @MainActor in await session.stop(log: { print("vm: \($0)") }) }
            }
            sigint.resume()
            let status = await session.waitUntilExit()
            sigint.cancel()
            Output.line("vm: QEMU exited with status \(status)")
            if status != 0 { throw ExitCode(status) }
        }
    }

    struct Stop: AsyncParsableCommand {
        static let configuration = CommandConfiguration(commandName: "stop", abstract: "Stop the running VM (QMP quit, then signals).")
        @OptionGroup var profileOption: ProfileOption

        func run() async throws {
            let paths = try profileOption.paths()
            try await VMController.stop(paths.state(for: profileOption.profile), log: Output.stdoutLogger)
        }
    }

    struct Status: AsyncParsableCommand {
        static let configuration = CommandConfiguration(commandName: "status", abstract: "Show whether the VM is running.")
        @OptionGroup var profileOption: ProfileOption
        @Flag(name: .long, help: "Print JSON.")
        var json = false

        func run() async throws {
            let paths = try profileOption.paths()
            let status = VMController.status(paths.state(for: profileOption.profile))
            if json { try Output.json(status); return }
            Output.line("profile: \(status.profile)")
            Output.line("status:  \(status.alive ? "running (pid \(status.pid ?? 0)\(status.qmpStatus.map { ", \($0)" } ?? ""))" : "not running")")
            Output.line("serial:  \(status.serialLog) (\(status.serialBytes) bytes)")
        }
    }

    struct Serial: AsyncParsableCommand {
        static let configuration = CommandConfiguration(commandName: "serial", abstract: "Print the serial log and follow it.")
        @OptionGroup var profileOption: ProfileOption
        @Flag(name: .long, help: "Print what is there and exit.")
        var noFollow = false

        func run() async throws {
            let paths = try profileOption.paths()
            let state = paths.state(for: profileOption.profile)
            guard FileManager.default.fileExists(atPath: state.serialLog.path) else {
                throw ValidationError("no serial log yet at \(state.serialLog.path)")
            }
            let tailer = LogTailer(url: state.serialLog)
            tailer.start()
            if noFollow {
                tailer.stop()
            }
            for await line in tailer.lines {
                print(line)
                fflush(stdout)
            }
        }
    }

    struct Doctor: AsyncParsableCommand {
        static let configuration = CommandConfiguration(commandName: "doctor", abstract: "Check QEMU, firmware, accelerator and display.")
        @OptionGroup var profileOption: ProfileOption

        func run() async throws {
            let host = await VMHost.detect(probe: .live())
            for check in VMDoctor.checks(host: host, paths: VMPaths.locate(), profileName: profileOption.profile) {
                Output.line("[\(check.passed ? "ok  " : "warn")] \(check.name): \(check.detail)")
            }
        }
    }
}
