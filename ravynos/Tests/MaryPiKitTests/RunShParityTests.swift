import Foundation
import Testing
@testable import MaryPiKit

/// `vm/run.sh` and `QEMUCommand` must produce the same argv. The script's
/// `--print-argv` mode prints one argument per line without touching the host.
@Suite struct RunShParityTests {
    struct Case {
        var profile: String
        var accel: VMAccel
        var display: VMDisplayMode
        var memory: Int?
        var extra: [String]
    }

    @Test(arguments: [
        Case(profile: "qemu-virt", accel: .tcg, display: .window, memory: nil, extra: []),
        Case(profile: "qemu-virt", accel: .tcg, display: .serial, memory: 1024, extra: []),
        Case(profile: "qemu-virt", accel: .tcg, display: .none, memory: nil, extra: ["-d", "int"]),
        Case(profile: "qemu-virt-gicv3", accel: .hvf, display: .window, memory: nil, extra: []),
    ])
    func matchesRunSh(_ c: Case) async throws {
        let paths = try #require(VMPaths.locate(environment: [:], currentDirectory: URL(fileURLWithPath: "/"), bundleResources: nil))
        let stateRoot = FileManager.default.temporaryDirectory.appending(path: "marypi-parity-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: stateRoot) }
        let env: [String: String] = [
            VMHost.qemuEnvironmentKey: "/fake/qemu-system-aarch64",
            VMHost.firmwareCodeEnvironmentKey: "/fake/code.fd",
            VMHost.firmwareVarsEnvironmentKey: "/fake/vars.fd",
            VMPaths.environmentStateKey: stateRoot.path,
            "TMPDIR": "/tmp",
            "PATH": "/usr/bin:/bin",
        ]
        var args = ["run", "--print-argv", "--image", "/x/q.img", "--profile", c.profile, "--accel", c.accel.rawValue, "--display", c.display.rawValue]
        if let memory = c.memory { args += ["--memory", String(memory)] }
        if !c.extra.isEmpty { args += ["--"] + c.extra }
        let result = try await CommandRunner().run("/bin/sh", [paths.runScript.path] + args, environment: env)
        try result.checkSuccess("vm/run.sh --print-argv")

        let profile = try paths.loadProfile(c.profile)
        let state = VMStatePaths(directory: stateRoot.appending(path: c.profile), profile: c.profile, environment: env)
        let host = VMHost(os: HostProbe.currentOS, arch: HostProbe.currentArch, qemu: "/fake/qemu-system-aarch64", qemuVersion: nil,
                          firmware: VMFirmware(code: "/fake/code.fd", varsTemplate: "/fake/vars.fd", needsPadding: false, origin: "env"),
                          hvfAvailable: true, kvmAvailable: false, hostGICv2: nil, displayBackends: ["cocoa"], hasGUISession: true, isTerminal: false)
        let (mode, backend) = host.displayBackend(for: c.display)
        let spec = QEMULaunchSpec(profile: profile, image: URL(fileURLWithPath: "/x/q.img"),
                                  firmware: host.firmware!, state: state, accel: c.accel,
                                  cpu: host.cpu(for: c.accel, profile: profile), display: mode, displayBackend: backend,
                                  memoryMiB: c.memory, serialOnTerminal: false, extraArguments: c.extra)
        let expected = QEMUCommand(qemu: "/fake/qemu-system-aarch64", spec: spec).argvLines
        #expect(result.stdoutText == expected, "run.sh:\n\(result.stdoutText)\nSwift:\n\(expected)")
    }
}
