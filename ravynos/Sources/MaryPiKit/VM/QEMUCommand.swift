import Foundation

/// Inputs for one QEMU launch.
public struct QEMULaunchSpec: Sendable, Equatable {
    public var profile: VMProfile
    public var image: URL
    public var firmware: VMFirmware
    public var state: VMStatePaths
    public var accel: VMAccel
    public var cpu: String
    public var display: VMDisplayMode
    public var displayBackend: VMDisplayBackend
    public var memoryMiB: Int?
    /// Give QEMU the launching terminal for the serial console (stdio chardev).
    /// Only safe when QEMU will be the terminal's foreground process (a shell
    /// `exec`, as in vm/run.sh); a child of `swift run` is not, and would be
    /// stopped by SIGTTOU. Otherwise the serial goes to the log file and the
    /// launcher echoes it.
    public var serialOnTerminal: Bool
    public var extraArguments: [String]

    public init(profile: VMProfile, image: URL, firmware: VMFirmware, state: VMStatePaths, accel: VMAccel, cpu: String,
                display: VMDisplayMode, displayBackend: VMDisplayBackend, memoryMiB: Int? = nil,
                serialOnTerminal: Bool = false, extraArguments: [String] = []) {
        self.profile = profile
        self.image = image
        self.firmware = firmware
        self.state = state
        self.accel = accel
        self.cpu = cpu
        self.display = display
        self.displayBackend = displayBackend
        self.memoryMiB = memoryMiB
        self.serialOnTerminal = serialOnTerminal
        self.extraArguments = extraArguments
    }

    /// The firmware image actually passed to QEMU (padded copy when needed).
    public var firmwareCodePath: String {
        firmware.needsPadding ? state.paddedCode.path : firmware.code
    }
}

/// Pure argv builder. The order must match `vm/run.sh build_and_run`:
/// qemu, name, machine, accel, cpu, smp, memory, firmware, vars, disk,
/// devices, display group, qmp, pidfile, profile extras, passthrough.
public struct QEMUCommand: Sendable, Equatable {
    public let executable: String
    public let arguments: [String]

    public init(qemu: String, spec: QEMULaunchSpec) {
        executable = qemu
        var args: [String] = [
            "-name", "ravynOS-\(spec.profile.name)",
            "-M", spec.profile.machineArgument,
            "-accel", spec.accel.rawValue,
            "-cpu", spec.cpu,
            "-smp", String(spec.profile.smp),
            "-m", String(spec.memoryMiB ?? spec.profile.memoryMiB),
            "-drive", "if=pflash,format=raw,readonly=on,file=\(spec.firmwareCodePath)",
            "-drive", "if=pflash,format=raw,file=\(spec.state.varsFlash.path)",
            "-drive", "file=\(spec.image.path),format=raw,if=none,id=hd0",
            "-device", "\(spec.profile.diskDevice),drive=hd0",
        ]
        for device in spec.profile.devices {
            args += ["-device", device]
        }
        let log = spec.state.serialLog.path
        switch spec.display {
        case .window:
            args += ["-display", spec.displayBackend.rawValue]
            if spec.serialOnTerminal {
                args += ["-chardev", "stdio,id=serial0,logfile=\(log),signal=off"]
            } else {
                args += ["-chardev", "file,id=serial0,path=\(log)"]
            }
            args += ["-serial", "chardev:serial0"]
        case .serial:
            if spec.serialOnTerminal {
                args += ["-display", "none",
                         "-chardev", "stdio,id=serial0,mux=on,logfile=\(log),signal=off",
                         "-serial", "chardev:serial0",
                         "-mon", "chardev=serial0,mode=readline"]
            } else {
                // No terminal to hand to QEMU: log to the file and let the launcher echo it.
                args += ["-display", "none", "-monitor", "none",
                         "-chardev", "file,id=serial0,path=\(log)",
                         "-serial", "chardev:serial0"]
            }
        case .none:
            args += ["-display", "none", "-monitor", "none",
                     "-chardev", "file,id=serial0,path=\(log)",
                     "-serial", "chardev:serial0"]
        }
        args += ["-qmp", "unix:\(spec.state.qmpSocketPath),server=on,wait=off",
                 "-pidfile", spec.state.pidFile.path]
        args += spec.profile.extraArgs
        args += spec.extraArguments
        arguments = args
    }

    public var argv: [String] { [executable] + arguments }

    /// Shell-quoted single line, for `--dry-run`.
    public var commandLine: String {
        argv.map(FlashScript.shellQuote).joined(separator: " ")
    }

    /// One argument per line, matching `vm/run.sh run --print-argv`.
    public var argvLines: String {
        argv.joined(separator: "\n") + "\n"
    }
}
