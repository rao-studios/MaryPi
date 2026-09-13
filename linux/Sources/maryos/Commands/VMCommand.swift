import AppKit
import ArgumentParser
import AVFoundation
import Foundation
import MaryOSKit
import Virtualization

struct VMCommand: ParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "vm",
        abstract: "Boot the built MaryOS image in Apple's Virtualization.framework, in a window or headless.",
        subcommands: [Run.self, Stop.self, Status.self, Serial.self, Reset.self],
        defaultSubcommand: Status.self
    )

    struct Run: ParsableCommand {
        static let configuration = CommandConfiguration(commandName: "run", abstract: "Boot the VM image (out/maryos-*-vm.img with out/vm/Image) in its own window.")

        @OptionGroup var kit: KitOptions

        @Flag(name: .long, help: "No window: the guest only has its serial console.")
        var headless = false

        @Flag(name: .long, help: "Attach this terminal to the guest's serial console (hvc0). Ctrl-] stops the VM.")
        var console = false

        @Flag(name: .long, help: "Boot to the Liquid Platinum desktop (systemd.unit=graphical.target) instead of the login prompt.")
        var desktop = false

        @Flag(name: .long, help: "With --desktop: run the desktop from out/ui over virtiofs and restart it whenever `make ui` replaces it (maryos.ui=dev).")
        var dev = false

        @Flag(name: .long, help: "Give the guest this Mac's microphone (its sound device gains an input), so Mary can hear you. macOS asks once.")
        var microphone = false

        @Option(name: .long, help: "Guest memory in MiB (default \(VMSpec.defaultMemoryMiB)).")
        var memory: Int?

        @Option(name: .long, help: "Virtual CPUs (default: half the host's cores, at most 4).")
        var cpus: Int?

        @Option(name: .long, help: "Share a host directory over virtiofs: tag=/path, /path (tag = folder name), suffix :ro for read-only. Repeatable; out/ is always shared as \(VMSpec.defaultShareTag).")
        var share: [String] = []

        @Option(name: .long, help: "Size of the VM's disk in GiB, applied when the disk is (re)created (default VM_DISK_GIB from distro.conf).")
        var diskSize: Int?

        @Flag(name: .long, help: "Start from a fresh copy of the built image, discarding this VM's disk.")
        var fresh = false

        @Flag(name: .long, help: "Do not echo the serial log on this terminal.")
        var quiet = false

        @Flag(name: .long, help: "Print what would be booted and exit.")
        var dryRun = false

        func run() throws {
            // Virtualization needs an entitled binary; `swift run` gives an
            // unsigned one. Sign ourselves and start over when needed.
            do {
                try SelfEntitlement.ensure { line in
                    FileHandle.standardError.write(Data("maryos: \(line)\n".utf8))
                }
            } catch {
                throw ValidationError(error.localizedDescription)
            }
            if dev && !desktop { throw ValidationError("--dev needs --desktop") }
            if desktop && headless { throw ValidationError("--desktop needs a window; drop --headless") }
            if microphone && headless { throw ValidationError("--microphone needs a window; drop --headless") }
            let mode: VMBootMode = desktop ? .desktop(dev: dev) : .console
            let paths = try kit.paths()
            let config = try kit.config(paths)
            let artifacts = BuildArtifacts.locate(target: .vm, config: config, paths: paths)
            if dev, !UIArtifacts.locate(paths: paths).isBuilt {
                Output.line("vm: warning, \(paths.uiBinary.path) is not built (make ui); the guest falls back to the desktop in the image")
            }
            let state = paths.state(for: .vm)
            if let pid = VMController.runningPID(state) {
                throw ValidationError("a VM is already running (pid \(pid)); maryos vm stop")
            }
            var shares = [SharedDirectory(tag: VMSpec.defaultShareTag, url: paths.outDirectory)]
            for text in share {
                do { shares.append(try SharedDirectory.parse(text)) } catch { throw ValidationError(error.localizedDescription) }
            }
            if !dryRun {
                do {
                    try FileManager.default.createDirectory(at: paths.outDirectory, withIntermediateDirectories: true)
                    try VMStateManager.prepare(state, artifacts: artifacts, fresh: fresh, diskSize: Int64(diskSize ?? config.vmDiskGiB) << 30, log: Output.stdoutLogger)
                } catch {
                    throw ValidationError(error.localizedDescription)
                }
            }
            let bootInfoURL = FileManager.default.fileExists(atPath: state.bootInfo.path) ? state.bootInfo : (artifacts.bootInfo ?? state.bootInfo)
            let boot: VMBootInfo
            do { boot = try VMBootInfo.load(from: bootInfoURL) } catch { throw ValidationError("\(error.localizedDescription); build the vm target first") }
            let spec = VMSpec(
                name: config.fullName, cpus: cpus ?? VMSpec.defaultCPUs, memoryMiB: memory ?? VMSpec.defaultMemoryMiB,
                disk: state.disk, kernel: state.kernel, initrd: state.initrd, commandLine: mode.commandLine(base: boot.cmdline),
                macAddress: dryRun ? nil : try VMStateManager.macAddress(state),
                sharedDirectories: shares, headless: headless, bootMode: mode, microphone: microphone
            )
            if dryRun {
                Output.line("vm: \(spec.name): \(spec.summary)")
                Output.line("vm: boot mode \(mode.title)")
                Output.line("vm: kernel \(boot.kernelVersion) (built \(boot.built)), cmdline: \(spec.commandLine)")
                for directory in shares { Output.line("vm: share \(directory.tag) = \(directory.url.path)\(directory.readOnly ? " (read-only)" : "")") }
                Output.line("vm: state \(state.directory.path)")
                return
            }
            if microphone { MicrophoneAccess.ensure() }
            let status = try MainActor.assumeIsolated {
                try VMLauncher.run(spec: spec, state: state, echoSerial: !quiet && !console, console: console, windowed: !headless)
            }
            if status != 0 { throw ExitCode(status) }
        }
    }

    struct Stop: ParsableCommand {
        static let configuration = CommandConfiguration(commandName: "stop", abstract: "Stop the running VM (asks the guest to shut down, then forces it).")
        @OptionGroup var kit: KitOptions

        func run() throws {
            let state = try kit.paths().state(for: .vm)
            try runBlocking { try await VMController.stop(state, log: Output.stdoutLogger) }
        }
    }

    struct Status: ParsableCommand {
        static let configuration = CommandConfiguration(commandName: "status", abstract: "Show whether the VM is running and where its files are.")
        @OptionGroup var kit: KitOptions
        @Flag(name: .long, help: "Print JSON.")
        var json = false

        func run() throws {
            let state = try kit.paths().state(for: .vm)
            let status = VMController.status(state)
            if json { try Output.json(status); return }
            Output.line("target:  \(status.target)")
            Output.line("status:  \(status.alive ? "running (pid \(status.pid ?? 0))" : "not running")")
            Output.line("disk:    \(status.diskExists ? "\(status.disk) (\(ByteCountFormatter.string(fromByteCount: status.diskBytes ?? 0, countStyle: .file)))" : "none yet (maryos vm run clones the built image)")")
            Output.line("serial:  \(status.serialLog) (\(status.serialBytes) bytes)")
        }
    }

    struct Serial: ParsableCommand {
        static let configuration = CommandConfiguration(commandName: "serial", abstract: "Print the serial log and follow it.")
        @OptionGroup var kit: KitOptions
        @Flag(name: .long, help: "Print what is there and exit.")
        var noFollow = false

        func run() throws {
            let state = try kit.paths().state(for: .vm)
            guard FileManager.default.fileExists(atPath: state.serialLog.path) else {
                throw ValidationError("no serial log yet at \(state.serialLog.path)")
            }
            let follow = !noFollow
            try runBlocking {
                let tailer = LogTailer(url: state.serialLog)
                tailer.start()
                if !follow { tailer.stop() }
                for await line in tailer.lines {
                    print(line)
                    fflush(stdout)
                }
            }
        }
    }

    struct Reset: ParsableCommand {
        static let configuration = CommandConfiguration(commandName: "reset", abstract: "Discard the VM's disk; the next run starts from a fresh copy of the built image.")
        @OptionGroup var kit: KitOptions

        func run() throws {
            let state = try kit.paths().state(for: .vm)
            if let pid = VMController.runningPID(state) {
                throw ValidationError("the VM is running (pid \(pid)); stop it first")
            }
            try VMStateManager.reset(state)
            Output.line("vm: removed \(state.disk.path) and its kernel files")
        }
    }
}

/// macOS asks the person once whether this Mac's microphone may be used. Until they agree the
/// guest hears silence; the VM still boots.
enum MicrophoneAccess {
    static func ensure() {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized:
            return
        case .notDetermined:
            let granted = (try? runBlocking { await AVCaptureDevice.requestAccess(for: .audio) }) ?? false
            if !granted { Output.line("vm: warning, microphone access was not granted; the guest will hear silence") }
        default:
            Output.line("vm: warning, this app may not use the microphone (System Settings › Privacy & Security › Microphone); the guest will hear silence")
        }
    }
}

/// Boots a VM from the synchronous CLI: headless runs pump the run loop
/// through `runBlocking`; a window needs AppKit's own loop (`NSApp.run()`),
/// called here from the real main thread.
@MainActor
enum VMLauncher {
    static func run(spec: VMSpec, state: VMStatePaths, echoSerial: Bool, console: Bool, windowed: Bool) throws -> Int32 {
        let runner = VMRunner(paths: state)
        let log: Logger = { line in
            print("vm: \(line)")
            fflush(stdout)
        }
        let stopVM: @Sendable () -> Void = {
            Task { @MainActor in await runner.stop(log: log) }
        }
        let terminal: TerminalConsole? = console ? TerminalConsole(onEscape: stopVM) : nil
        let machine: VZVirtualMachine
        do {
            machine = try runner.load(spec, consoleInput: terminal?.guestInput, consoleOutput: terminal == nil ? nil : FileHandle.standardOutput)
        } catch {
            throw ValidationError(error.localizedDescription)
        }
        let signals = installSignalHandlers(stopVM)
        defer { withExtendedLifetime(signals) {} }
        var serial: Logger?
        if echoSerial {
            serial = { line in
                print(line)
                fflush(stdout)
            }
        }

        if windowed {
            runWindowed(runner: runner, machine: machine, spec: spec, state: state, terminal: terminal, serial: serial, log: log, stopVM: stopVM)
        }
        let final = try runBlocking {
            try await runner.start(serial: serial, log: log)
            announce(terminal: terminal, windowed: false)
            return await runner.waitUntilStopped()
        }
        return report(final, state: state, terminal: terminal)
    }

    private static func runWindowed(runner: VMRunner, machine: VZVirtualMachine, spec: VMSpec, state: VMStatePaths,
                                    terminal: TerminalConsole?, serial: Logger?, log: @escaping Logger,
                                    stopVM: @escaping @Sendable () -> Void) -> Never {
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        let window = VMWindowController(machine: machine, title: spec.name, width: spec.displayWidth, height: spec.displayHeight)
        window.onClose = { stopVM() }
        window.showWindow(nil)
        app.activate(ignoringOtherApps: true)
        Task { @MainActor in
            do {
                try await runner.start(serial: serial, log: log)
            } catch {
                Output.line("vm: \(error.localizedDescription)")
                terminal?.restore()
                exit(1)
            }
            announce(terminal: terminal, windowed: true)
            let final = await runner.waitUntilStopped()
            let status = report(final, state: state, terminal: terminal)
            withExtendedLifetime(window) {}
            exit(status)
        }
        app.run()
        exit(0)
    }

    private static func announce(terminal: TerminalConsole?, windowed: Bool) {
        if let terminal {
            terminal.enterRawMode()
            Output.line("vm: this terminal is the guest's serial console; Ctrl-] stops the VM")
        } else if windowed {
            Output.line("vm: close the window, press Ctrl-C, or run `maryos vm stop` to shut the guest down")
        } else {
            Output.line("vm: Ctrl-C or `maryos vm stop` shuts the guest down")
        }
    }

    private static func report(_ final: VMRunner.State, state: VMStatePaths, terminal: TerminalConsole?) -> Int32 {
        terminal?.restore()
        switch final {
        case .stopped(let why):
            Output.line("vm: stopped (\(why)); serial log \(state.serialLog.path)")
            return 0
        case .failed(let message):
            Output.line("vm: failed: \(message); serial log \(state.serialLog.path)")
            return 1
        default:
            return 0
        }
    }
}
