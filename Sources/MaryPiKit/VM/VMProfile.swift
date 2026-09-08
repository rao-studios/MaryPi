import Foundation

/// Parser for the `KEY=VALUE` profile files in `vm/profiles/`, which are also
/// sourced by `vm/run.sh`. Supports `#` comments, blank lines, and single or
/// double quoted values.
public enum ConfParser {
    public static func parse(_ text: String) throws -> [String: String] {
        var result: [String: String] = [:]
        for (index, rawLine) in text.split(omittingEmptySubsequences: false, whereSeparator: \.isNewline).enumerated() {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            guard let equals = line.firstIndex(of: "=") else {
                throw MaryPiError("profile line \(index + 1) is not KEY=VALUE: \(line)")
            }
            let key = line[..<equals].trimmingCharacters(in: .whitespaces)
            var value = line[line.index(after: equals)...].trimmingCharacters(in: .whitespaces)
            guard !key.isEmpty, key.allSatisfy({ $0.isLetter || $0.isNumber || $0 == "_" }) else {
                throw MaryPiError("profile line \(index + 1) has an invalid key: \(line)")
            }
            if let first = value.first, first == "\"" || first == "'" {
                // quoted value: everything up to the closing quote, rest ignored (comments)
                let body = value.dropFirst()
                if let close = body.firstIndex(of: first) {
                    value = String(body[..<close])
                } else {
                    throw MaryPiError("profile line \(index + 1) has an unterminated quote: \(line)")
                }
            } else if let comment = value.firstIndex(of: "#") {
                value = value[..<comment].trimmingCharacters(in: .whitespaces)
            }
            result[key] = value
        }
        return result
    }
}

/// A VM definition: which QEMU machine, interrupt controller, CPUs, memory
/// and devices to use. Mirrors the keys in `vm/profiles/<name>.conf`.
public struct VMProfile: Codable, Sendable, Equatable {
    public var name: String
    public var machine: String
    public var gicVersion: Int
    public var machineOptions: String
    public var cpuEmulated: String
    public var cpuAccelerated: String
    public var smp: Int
    public var memoryMiB: Int
    public var diskDevice: String
    public var devices: [String]
    public var extraArgs: [String]

    public init(name: String, machine: String = "virt", gicVersion: Int = 2, machineOptions: String = "acpi=off",
                cpuEmulated: String = "cortex-a76", cpuAccelerated: String = "host", smp: Int = 1, memoryMiB: Int = 2048,
                diskDevice: String = "virtio-blk-device",
                devices: [String] = ["ramfb", "qemu-xhci", "usb-kbd", "usb-tablet"], extraArgs: [String] = []) {
        self.name = name
        self.machine = machine
        self.gicVersion = gicVersion
        self.machineOptions = machineOptions
        self.cpuEmulated = cpuEmulated
        self.cpuAccelerated = cpuAccelerated
        self.smp = smp
        self.memoryMiB = memoryMiB
        self.diskDevice = diskDevice
        self.devices = devices
        self.extraArgs = extraArgs
    }

    /// Build from parsed `KEY=VALUE` pairs; `fallbackName` is the file's base name.
    public init(conf: [String: String], fallbackName: String) throws {
        func required(_ key: String) throws -> String {
            guard let value = conf[key], !value.isEmpty else { throw MaryPiError("profile \(fallbackName) does not set \(key)") }
            return value
        }
        func integer(_ key: String) throws -> Int {
            guard let value = Int(try required(key)) else { throw MaryPiError("profile \(fallbackName): \(key) is not a number") }
            return value
        }
        name = conf["VM_NAME"].flatMap { $0.isEmpty ? nil : $0 } ?? fallbackName
        machine = try required("VM_MACHINE")
        gicVersion = try integer("VM_GIC")
        machineOptions = conf["VM_MACHINE_OPTS"] ?? ""
        cpuEmulated = try required("VM_CPU_TCG")
        cpuAccelerated = try required("VM_CPU_HW")
        smp = try integer("VM_SMP")
        memoryMiB = try integer("VM_MEMORY_MIB")
        diskDevice = try required("VM_DISK_DEVICE")
        devices = try required("VM_DEVICES").split(whereSeparator: \.isWhitespace).map(String.init)
        extraArgs = (conf["VM_EXTRA_ARGS"] ?? "").split(whereSeparator: \.isWhitespace).map(String.init)
    }

    public static func load(from url: URL) throws -> VMProfile {
        let text = try String(contentsOf: url, encoding: .utf8)
        let name = url.deletingPathExtension().lastPathComponent
        return try VMProfile(conf: ConfParser.parse(text), fallbackName: name)
    }

    /// The `-M` argument, e.g. `virt,gic-version=2,acpi=off`.
    public var machineArgument: String {
        var value = "\(machine),gic-version=\(gicVersion)"
        if !machineOptions.isEmpty { value += ",\(machineOptions)" }
        return value
    }
}
