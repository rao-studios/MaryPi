import Foundation
import Virtualization

/// Turns a `VMSpec` into a validated `VZVirtualMachineConfiguration`:
/// direct kernel boot (VZLinuxBootLoader with the extracted arm64 Image and
/// the initrd), a virtio disk, NAT networking, a virtio console for the
/// serial log, virtio-gpu plus USB keyboard, pointer and sound output when
/// there is a window, entropy, a memory balloon and virtiofs shares.
public enum VMConfigurationBuilder {
    /// The assembled and validated configuration. Validation needs the
    /// virtualization entitlement, so unsigned processes fail here.
    public static func make(_ spec: VMSpec, serialInput: FileHandle?, serialOutput: FileHandle) throws -> VZVirtualMachineConfiguration {
        let configuration = try assemble(spec, serialInput: serialInput, serialOutput: serialOutput)
        do {
            try configuration.validate()
        } catch {
            throw MaryOSError("VM configuration is invalid: \(error.localizedDescription)")
        }
        return configuration
    }

    /// The configuration without validation.
    public static func assemble(_ spec: VMSpec, serialInput: FileHandle?, serialOutput: FileHandle) throws -> VZVirtualMachineConfiguration {
        let configuration = VZVirtualMachineConfiguration()

        let bootLoader = VZLinuxBootLoader(kernelURL: spec.kernel)
        bootLoader.initialRamdiskURL = spec.initrd
        bootLoader.commandLine = spec.commandLine
        configuration.bootLoader = bootLoader

        configuration.cpuCount = min(max(spec.cpus, VZVirtualMachineConfiguration.minimumAllowedCPUCount), VZVirtualMachineConfiguration.maximumAllowedCPUCount)
        let requested = UInt64(spec.memoryMiB) << 20
        configuration.memorySize = min(max(requested, VZVirtualMachineConfiguration.minimumAllowedMemorySize), VZVirtualMachineConfiguration.maximumAllowedMemorySize)

        let serial = VZVirtioConsoleDeviceSerialPortConfiguration()
        serial.attachment = VZFileHandleSerialPortAttachment(fileHandleForReading: serialInput, fileHandleForWriting: serialOutput)
        configuration.serialPorts = [serial]

        let diskAttachment = try VZDiskImageStorageDeviceAttachment(url: spec.disk, readOnly: false)
        configuration.storageDevices = [VZVirtioBlockDeviceConfiguration(attachment: diskAttachment)]

        let network = VZVirtioNetworkDeviceConfiguration()
        network.attachment = VZNATNetworkDeviceAttachment()
        if let mac = spec.macAddress {
            guard let address = VZMACAddress(string: mac) else { throw MaryOSError("invalid MAC address \(mac)") }
            network.macAddress = address
        }
        configuration.networkDevices = [network]

        configuration.entropyDevices = [VZVirtioEntropyDeviceConfiguration()]
        configuration.memoryBalloonDevices = [VZVirtioTraditionalMemoryBalloonDeviceConfiguration()]

        if !spec.headless {
            let graphics = VZVirtioGraphicsDeviceConfiguration()
            graphics.scanouts = [VZVirtioGraphicsScanoutConfiguration(widthInPixels: spec.displayWidth, heightInPixels: spec.displayHeight)]
            configuration.graphicsDevices = [graphics]
            configuration.keyboards = [VZUSBKeyboardConfiguration()]
            configuration.pointingDevices = [VZUSBScreenCoordinatePointingDeviceConfiguration()]
            // The Media Player's audio reaches the Mac's speakers (snd_virtio in the guest).
            // Output only: an input stream would need the microphone entitlement.
            let sound = VZVirtioSoundDeviceConfiguration()
            let output = VZVirtioSoundDeviceOutputStreamConfiguration()
            output.sink = VZHostAudioOutputStreamSink()
            sound.streams = [output]
            configuration.audioDevices = [sound]
        }

        var shares: [VZDirectorySharingDeviceConfiguration] = []
        for directory in spec.sharedDirectories {
            do {
                try VZVirtioFileSystemDeviceConfiguration.validateTag(directory.tag)
            } catch {
                throw MaryOSError("share tag \(directory.tag) is not valid: \(error.localizedDescription)")
            }
            let device = VZVirtioFileSystemDeviceConfiguration(tag: directory.tag)
            device.share = VZSingleDirectoryShare(directory: VZSharedDirectory(url: directory.url, readOnly: directory.readOnly))
            shares.append(device)
        }
        configuration.directorySharingDevices = shares
        return configuration
    }
}
