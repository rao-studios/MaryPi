import Foundation

public enum MachOArch: Hashable, Sendable, CustomStringConvertible {
    case x86_64
    case arm64
    case arm64e
    case i386
    case arm
    case other(UInt32)

    public var description: String {
        switch self {
        case .x86_64: return "x86_64"
        case .arm64: return "arm64"
        case .arm64e: return "arm64e"
        case .i386: return "i386"
        case .arm: return "arm"
        case .other(let value): return String(format: "cputype 0x%08x", value)
        }
    }

    public var isARM64: Bool {
        self == .arm64 || self == .arm64e
    }
}

/// Just enough Mach-O parsing to tell which architectures a file contains.
public enum MachO {
    static let cpuTypeX86_64: UInt32 = 0x0100_0007
    static let cpuTypeARM64: UInt32 = 0x0100_000C
    static let cpuTypeI386: UInt32 = 0x0000_0007
    static let cpuTypeARM: UInt32 = 0x0000_000C

    public static func architectures(at url: URL) throws -> Set<MachOArch> {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        let head = try handle.read(upToCount: 4096) ?? Data()
        return architectures(in: head)
    }

    public static func architectures(in data: Data) -> Set<MachOArch> {
        let bytes = [UInt8](data)
        guard bytes.count >= 8 else { return [] }

        func u32(_ offset: Int, bigEndian: Bool) -> UInt32? {
            guard offset + 4 <= bytes.count else { return nil }
            let b0 = UInt32(bytes[offset]), b1 = UInt32(bytes[offset + 1])
            let b2 = UInt32(bytes[offset + 2]), b3 = UInt32(bytes[offset + 3])
            return bigEndian
                ? (b0 << 24) | (b1 << 16) | (b2 << 8) | b3
                : (b3 << 24) | (b2 << 16) | (b1 << 8) | b0
        }

        guard let magic = u32(0, bigEndian: false) else { return [] }
        switch magic {
        case 0xFEED_FACF, 0xFEED_FACE:
            guard let type = u32(4, bigEndian: false), let subtype = u32(8, bigEndian: false) else { return [] }
            return [arch(cputype: type, cpusubtype: subtype)]
        case 0xCFFA_EDFE, 0xCEFA_EDFE:
            guard let type = u32(4, bigEndian: true), let subtype = u32(8, bigEndian: true) else { return [] }
            return [arch(cputype: type, cpusubtype: subtype)]
        case 0xBEBA_FECA, 0xBFBA_FECA:
            let entrySize = magic == 0xBEBA_FECA ? 20 : 32
            guard let count = u32(4, bigEndian: true), count > 0, count <= 32 else { return [] }
            var result = Set<MachOArch>()
            for index in 0..<Int(count) {
                let base = 8 + index * entrySize
                guard let type = u32(base, bigEndian: true), let subtype = u32(base + 4, bigEndian: true) else { break }
                result.insert(arch(cputype: type, cpusubtype: subtype))
            }
            return result
        default:
            return []
        }
    }

    static func arch(cputype: UInt32, cpusubtype: UInt32) -> MachOArch {
        switch cputype {
        case cpuTypeX86_64: return .x86_64
        case cpuTypeARM64: return (cpusubtype & 0xFF) == 2 ? .arm64e : .arm64
        case cpuTypeI386: return .i386
        case cpuTypeARM: return .arm
        default: return .other(cputype)
        }
    }

    /// True when the file exists and contains arm64 or arm64e code.
    public static func isARM64(_ url: URL) -> Bool {
        guard let archs = try? architectures(at: url) else { return false }
        return archs.contains { $0.isARM64 }
    }
}
