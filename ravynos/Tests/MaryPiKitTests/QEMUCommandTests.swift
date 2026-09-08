import Foundation
import Testing
@testable import MaryPiKit

@Suite struct QEMUCommandTests {
    let firmware = VMFirmware(code: "/fw/code.fd", varsTemplate: "/fw/vars.fd", needsPadding: false, origin: "test")
    let state = VMStatePaths(directory: URL(fileURLWithPath: "/st/qemu-virt"), profile: "qemu-virt", environment: [:])

    @Test func macWindowFromTerminal() {
        let spec = QEMULaunchSpec(profile: VMProfile(name: "qemu-virt-gicv3", gicVersion: 3), image: URL(fileURLWithPath: "/img/q.img"),
                                  firmware: firmware, state: state, accel: .hvf, cpu: "host", display: .window, displayBackend: .cocoa,
                                  serialOnTerminal: true)
        let command = QEMUCommand(qemu: "/opt/homebrew/bin/qemu-system-aarch64", spec: spec)
        #expect(command.arguments == [
            "-name", "ravynOS-qemu-virt-gicv3", "-M", "virt,gic-version=3,acpi=off", "-accel", "hvf", "-cpu", "host",
            "-smp", "1", "-m", "2048",
            "-drive", "if=pflash,format=raw,readonly=on,file=/fw/code.fd",
            "-drive", "if=pflash,format=raw,file=/st/qemu-virt/vars.fd",
            "-drive", "file=/img/q.img,format=raw,if=none,id=hd0", "-device", "virtio-blk-device,drive=hd0",
            "-device", "ramfb", "-device", "qemu-xhci", "-device", "usb-kbd", "-device", "usb-tablet",
            "-display", "cocoa", "-chardev", "stdio,id=serial0,logfile=/st/qemu-virt/serial.log,signal=off", "-serial", "chardev:serial0",
            "-qmp", "unix:/st/qemu-virt/qmp.sock,server=on,wait=off", "-pidfile", "/st/qemu-virt/qemu.pid",
        ])
    }

    @Test func appWindowUsesFileChardev() {
        let spec = QEMULaunchSpec(profile: VMProfile(name: "qemu-virt"), image: URL(fileURLWithPath: "/img/q.img"), firmware: firmware,
                                  state: state, accel: .tcg, cpu: "cortex-a76", display: .window, displayBackend: .cocoa)
        let args = QEMUCommand(qemu: "/q", spec: spec).arguments
        #expect(args.contains("file,id=serial0,path=/st/qemu-virt/serial.log"))
        #expect(!args.contains("-monitor"))
    }

    @Test func serialAndNoneModes() {
        let base = QEMULaunchSpec(profile: VMProfile(name: "qemu-virt"), image: URL(fileURLWithPath: "/img/q.img"), firmware: firmware,
                                  state: state, accel: .tcg, cpu: "cortex-a76", display: .serial, displayBackend: .none)
        let serialNoTTY = QEMUCommand(qemu: "/q", spec: base).arguments
        #expect(serialNoTTY.contains("file,id=serial0,path=/st/qemu-virt/serial.log"))
        #expect(!serialNoTTY.contains("-mon"))
        var ttySpec = base
        ttySpec.serialOnTerminal = true
        let serial = QEMUCommand(qemu: "/q", spec: ttySpec).arguments
        #expect(serial.contains("stdio,id=serial0,mux=on,logfile=/st/qemu-virt/serial.log,signal=off"))
        #expect(serial.contains("chardev=serial0,mode=readline"))
        var noneSpec = base
        noneSpec.display = .none
        let none = QEMUCommand(qemu: "/q", spec: noneSpec).arguments
        #expect(none.contains("-monitor") && none.contains("none"))
        #expect(none.contains("file,id=serial0,path=/st/qemu-virt/serial.log"))
    }

    @Test func memoryPaddingAndExtras() {
        var profile = VMProfile(name: "qemu-virt")
        profile.extraArgs = ["-d", "int"]
        let padded = VMFirmware(code: "/usr/share/qemu-efi-aarch64/QEMU_EFI.fd", varsTemplate: nil, needsPadding: true, origin: "t")
        let spec = QEMULaunchSpec(profile: profile, image: URL(fileURLWithPath: "/img/q.img"), firmware: padded, state: state,
                                  accel: .tcg, cpu: "cortex-a76", display: .none, displayBackend: .none, memoryMiB: 1024,
                                  extraArguments: ["-monitor", "telnet:127.0.0.1:4455,server,nowait"])
        let args = QEMUCommand(qemu: "/q", spec: spec).arguments
        #expect(args.contains("if=pflash,format=raw,readonly=on,file=/st/qemu-virt/code.fd"))
        #expect(args[args.firstIndex(of: "-m")! + 1] == "1024")
        #expect(args.suffix(4) == ["-d", "int", "-monitor", "telnet:127.0.0.1:4455,server,nowait"])
    }

    @Test func quoting() {
        let spec = QEMULaunchSpec(profile: VMProfile(name: "qemu-virt"), image: URL(fileURLWithPath: "/my images/q.img"), firmware: firmware,
                                  state: state, accel: .tcg, cpu: "cortex-a76", display: .none, displayBackend: .none)
        let command = QEMUCommand(qemu: "/q", spec: spec)
        #expect(command.commandLine.contains("'file=/my images/q.img,format=raw,if=none,id=hd0'"))
        #expect(command.argvLines.hasPrefix("/q\n-name\n"))
    }

    @Test func statePathsFallBackToShortSocket() {
        let long = URL(fileURLWithPath: "/" + String(repeating: "x", count: 120))
        let paths = VMStatePaths(directory: long, profile: "p", environment: ["TMPDIR": "/tmp/t/"])
        #expect(paths.usesFallbackSocket)
        #expect(paths.qmpSocketPath == "/tmp/t//marypi-vm-p.sock")
        let short = VMStatePaths(directory: URL(fileURLWithPath: "/s"), profile: "p", environment: [:])
        #expect(!short.usesFallbackSocket && short.qmpSocketPath == "/s/qmp.sock")
    }
}
