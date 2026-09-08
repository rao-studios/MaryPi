import Foundation

/// Doctor checks for the VM test bed: QEMU, firmware, accelerator, display.
public enum VMDoctor {
    public static func checks(host: VMHost, paths: VMPaths?, profileName: String = "qemu-virt") -> [DoctorCheck] {
        var checks: [DoctorCheck] = []
        checks.append(DoctorCheck(name: "qemu-system-aarch64", passed: host.qemu != nil, blocking: false,
                                  detail: host.qemu.map { "\($0)\(host.qemuVersion.map { " (\($0))" } ?? "")" }
                                      ?? "not found (brew install qemu / apt install qemu-system-arm); VM testing unavailable"))
        if let firmware = host.firmware {
            checks.append(DoctorCheck(name: "UEFI firmware", passed: true, blocking: false,
                                      detail: "\(firmware.code) [\(firmware.origin)]\(firmware.needsPadding ? ", padded to 64 MiB in the state dir" : "")"))
        } else {
            checks.append(DoctorCheck(name: "UEFI firmware", passed: false, blocking: false,
                                      detail: "no EDK2 AArch64 image found; set \(VMHost.firmwareCodeEnvironmentKey) or install qemu-efi-aarch64"))
        }
        if let paths {
            let profiles = paths.profileNames()
            checks.append(DoctorCheck(name: "VM profiles", passed: !profiles.isEmpty, blocking: false,
                                      detail: profiles.isEmpty ? "none in \(paths.profilesDirectory.path)" : "\(profiles.joined(separator: ", ")) in \(paths.vmDirectory.path)"))
            if let profile = try? paths.loadProfile(profileName) {
                let accel = (try? host.resolveAccel(requested: nil, profile: profile)) ?? .tcg
                var detail = "\(profile.name) (GICv\(profile.gicVersion)) → \(accel.rawValue), -cpu \(host.cpu(for: accel, profile: profile))"
                if host.hvfAvailable && accel != .hvf { detail += "; hvf available but needs a GICv3 profile (qemu-virt-gicv3)" }
                if host.kvmAvailable && accel != .kvm { detail += "; kvm available but the host GIC is not v2-compatible" }
                checks.append(DoctorCheck(name: "VM accelerator", passed: true, blocking: false, detail: detail))
                let display = host.displayBackend(for: .window)
                checks.append(DoctorCheck(name: "VM display", passed: display.backend != .none, blocking: false,
                                          detail: display.backend == .none ? "no window backend (no DISPLAY, or QEMU without gtk/sdl); serial only" : display.backend.rawValue))
                let status = VMController.status(paths.state(for: profileName))
                checks.append(DoctorCheck(name: "VM state", passed: true, blocking: false,
                                          detail: status.alive ? "running (pid \(status.pid ?? 0), \(status.qmpStatus ?? "no QMP"))" : "not running; state in \(paths.state(for: profileName).directory.path)"))
            }
        } else {
            checks.append(DoctorCheck(name: "VM profiles", passed: false, blocking: false,
                                      detail: "vm/ directory not found; set \(VMPaths.environmentDirKey) or run from the MaryPi checkout"))
        }
        return checks
    }
}
