import Foundation
import Testing
@testable import MaryPiKit

@Suite struct VMProfileTests {
    @Test func parsesConf() throws {
        let conf = try ConfParser.parse("""
        # comment
        VM_NAME=test
        VM_MACHINE = virt
        VM_DEVICES="ramfb usb-kbd"   # trailing comment
        VM_EXTRA_ARGS=''
        EMPTY=
        """)
        #expect(conf["VM_NAME"] == "test")
        #expect(conf["VM_MACHINE"] == "virt")
        #expect(conf["VM_DEVICES"] == "ramfb usb-kbd")
        #expect(conf["VM_EXTRA_ARGS"] == "")
        #expect(conf["EMPTY"] == "")
    }

    @Test func rejectsGarbage() {
        #expect(throws: MaryPiError.self) { _ = try ConfParser.parse("not a pair") }
        #expect(throws: MaryPiError.self) { _ = try ConfParser.parse("bad-key=1") }
    }

    @Test func buildsProfile() throws {
        let conf = try ConfParser.parse("""
        VM_NAME=x
        VM_MACHINE=virt
        VM_GIC=3
        VM_MACHINE_OPTS=acpi=off
        VM_CPU_TCG=cortex-a76
        VM_CPU_HW=host
        VM_SMP=2
        VM_MEMORY_MIB=1024
        VM_DISK_DEVICE=virtio-blk-device
        VM_DEVICES="ramfb qemu-xhci"
        VM_EXTRA_ARGS="-d int"
        """)
        let profile = try VMProfile(conf: conf, fallbackName: "fallback")
        #expect(profile.name == "x")
        #expect(profile.gicVersion == 3)
        #expect(profile.machineArgument == "virt,gic-version=3,acpi=off")
        #expect(profile.devices == ["ramfb", "qemu-xhci"])
        #expect(profile.extraArgs == ["-d", "int"])
        #expect(profile.smp == 2 && profile.memoryMiB == 1024)
    }

    @Test func missingKeyIsAnError() throws {
        let conf = try ConfParser.parse("VM_MACHINE=virt")
        #expect(throws: MaryPiError.self) { _ = try VMProfile(conf: conf, fallbackName: "p") }
    }

    @Test func committedProfilesLoad() throws {
        let paths = try #require(VMPaths.locate(environment: [:], currentDirectory: URL(fileURLWithPath: "/"), bundleResources: nil))
        let names = paths.profileNames()
        #expect(names.contains("qemu-virt"))
        #expect(names.contains("qemu-virt-gicv3"))
        let v2 = try paths.loadProfile("qemu-virt")
        let v3 = try paths.loadProfile("qemu-virt-gicv3")
        #expect(v2.gicVersion == 2 && v3.gicVersion == 3)
        #expect(v2.machineArgument == "virt,gic-version=2,acpi=off")
        #expect(v2.devices.contains("ramfb"))
    }
}
