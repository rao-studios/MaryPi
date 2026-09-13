import Foundation
import Testing
import Virtualization
@testable import MaryOSKit

@Suite struct VMConfigurationTests {
    func fakeFiles() throws -> (dir: URL, spec: VMSpec) {
        let dir = FileManager.default.temporaryDirectory.appending(path: "maryos-vm-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir.appending(path: "share"), withIntermediateDirectories: true)
        let disk = dir.appending(path: "disk.img")
        FileManager.default.createFile(atPath: disk.path, contents: nil)
        let handle = try FileHandle(forWritingTo: disk)
        try handle.truncate(atOffset: 16 << 20)
        try handle.close()
        try Data(repeating: 0, count: 4096).write(to: dir.appending(path: "Image"))
        try Data(repeating: 0, count: 4096).write(to: dir.appending(path: "initrd.img"))
        let spec = VMSpec(name: "Test", cpus: 2, memoryMiB: 1024, disk: disk, kernel: dir.appending(path: "Image"),
                          initrd: dir.appending(path: "initrd.img"), commandLine: "console=hvc0 root=LABEL=x",
                          macAddress: "02:00:00:aa:bb:cc",
                          sharedDirectories: [SharedDirectory(tag: "share", url: dir.appending(path: "share"))])
        return (dir, spec)
    }

    /// Validation needs the virtualization entitlement, which a test bundle
    /// does not have; the assembled configuration is checked instead.
    @Test func assemblesTheConfiguration() throws {
        guard VZVirtualMachine.isSupported else { return }
        let (dir, spec) = try fakeFiles()
        defer { try? FileManager.default.removeItem(at: dir) }
        let serial = dir.appending(path: "serial.log")
        FileManager.default.createFile(atPath: serial.path, contents: nil)
        let output = try FileHandle(forWritingTo: serial)
        defer { try? output.close() }
        let configuration = try VMConfigurationBuilder.assemble(spec, serialInput: nil, serialOutput: output)
        #expect(configuration.cpuCount == 2)
        #expect(configuration.memorySize == 1024 << 20)
        #expect(configuration.graphicsDevices.count == 1)
        #expect(configuration.keyboards.count == 1)
        #expect(configuration.directorySharingDevices.count == 1)
        #expect((configuration.bootLoader as? VZLinuxBootLoader)?.commandLine == "console=hvc0 root=LABEL=x")
        #expect(configuration.networkDevices.first?.macAddress.string == "02:00:00:aa:bb:cc")
        #expect((configuration.audioDevices.first as? VZVirtioSoundDeviceConfiguration)?.streams.count == 1)

        var listening = spec
        listening.microphone = true
        let heard = try VMConfigurationBuilder.assemble(listening, serialInput: nil, serialOutput: output)
        let sound = heard.audioDevices.first as? VZVirtioSoundDeviceConfiguration
        #expect(sound?.streams.count == 2)
        #expect(sound?.streams.contains { ($0 as? VZVirtioSoundDeviceInputStreamConfiguration)?.source is VZHostAudioInputStreamSource } == true)

        var headless = spec
        headless.headless = true
        let plain = try VMConfigurationBuilder.assemble(headless, serialInput: nil, serialOutput: output)
        #expect(plain.graphicsDevices.isEmpty)
        #expect(plain.keyboards.isEmpty)
    }

    @Test func rejectsBadInput() throws {
        guard VZVirtualMachine.isSupported else { return }
        let (dir, spec) = try fakeFiles()
        defer { try? FileManager.default.removeItem(at: dir) }
        let serial = dir.appending(path: "serial.log")
        FileManager.default.createFile(atPath: serial.path, contents: nil)
        let output = try FileHandle(forWritingTo: serial)
        defer { try? output.close() }
        var badMAC = spec
        badMAC.macAddress = "not-a-mac"
        #expect(throws: MaryOSError.self) { _ = try VMConfigurationBuilder.make(badMAC, serialInput: nil, serialOutput: output) }
        var badTag = spec
        badTag.sharedDirectories = [SharedDirectory(tag: "", url: dir)]
        #expect(throws: MaryOSError.self) { _ = try VMConfigurationBuilder.make(badTag, serialInput: nil, serialOutput: output) }
    }

    @Test func statePathsAndSummary() {
        let paths = VMStatePaths(directory: URL(fileURLWithPath: "/s/vm/vm"), target: .vm)
        #expect(paths.disk.path == "/s/vm/vm/disk.img")
        #expect(paths.kernel.path == "/s/vm/vm/Image")
        #expect(paths.pidFile.path == "/s/vm/vm/vm.pid")
        let spec = VMSpec(name: "MaryOS", cpus: 1, memoryMiB: 2048, disk: paths.disk, kernel: paths.kernel, initrd: nil, commandLine: "", headless: true)
        #expect(spec.summary == "1 CPU, 2048 MiB, disk disk.img, kernel Image, headless")
        let desktop = VMSpec(name: "MaryOS", cpus: 2, memoryMiB: 4096, disk: paths.disk, kernel: paths.kernel, initrd: nil,
                             commandLine: VMBootMode.desktop(dev: true).commandLine(base: "console=hvc0"), bootMode: .desktop(dev: true))
        #expect(desktop.commandLine == "console=hvc0 systemd.unit=graphical.target maryos.ui=dev")
        #expect(desktop.summary == "2 CPUs, 4096 MiB, disk disk.img, kernel Image, 1280x800 window, boots to the desktop (dev: out/ui over virtiofs)")
        #expect(VMBootMode.console.kernelArguments.isEmpty)
        let heard = VMSpec(name: "MaryOS", cpus: 2, memoryMiB: 4096, disk: paths.disk, kernel: paths.kernel, initrd: nil, commandLine: "", microphone: true)
        #expect(heard.summary == "2 CPUs, 4096 MiB, disk disk.img, kernel Image, 1280x800 window, microphone")
    }

    @Test func macAddressIsPersisted() throws {
        let dir = FileManager.default.temporaryDirectory.appending(path: "maryos-mac-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: dir) }
        let paths = VMStatePaths(directory: dir, target: .vm)
        let first = try VMStateManager.macAddress(paths)
        let second = try VMStateManager.macAddress(paths)
        #expect(first == second)
        #expect(VZMACAddress(string: first) != nil)
        #expect(FileManager.default.fileExists(atPath: paths.macAddressFile.path))
    }
}
