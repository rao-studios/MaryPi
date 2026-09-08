import Foundation

/// What an image is built for. Both targets share the base rootfs and the
/// disk layout; they differ in kernel flavour and boot files.
public enum ImageTarget: String, CaseIterable, Sendable, Codable, Hashable {
    /// Raspberry Pi 5: Ubuntu's raspi kernel, firmware files on partition 1.
    case pi5
    /// Apple Virtualization.framework: Ubuntu's generic kernel, booted directly from files on the host.
    case vm

    public var title: String {
        switch self {
        case .pi5: return "Raspberry Pi 5"
        case .vm: return "Virtual machine"
        }
    }
}
