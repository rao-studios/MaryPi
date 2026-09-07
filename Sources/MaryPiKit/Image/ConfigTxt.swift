import Foundation

/// Boot-time configuration MaryPi adds to the upstream config.txt.
public struct RavynBootConfig: Sendable, Equatable {
    public var enableUART = true
    public var uart2ndStage = true
    public var extra: [(key: String, value: String)] = []

    public init(enableUART: Bool = true, uart2ndStage: Bool = true, extra: [(key: String, value: String)] = []) {
        self.enableUART = enableUART
        self.uart2ndStage = uart2ndStage
        self.extra = extra
    }

    public static func == (lhs: RavynBootConfig, rhs: RavynBootConfig) -> Bool {
        lhs.enableUART == rhs.enableUART && lhs.uart2ndStage == rhs.uart2ndStage
            && lhs.extra.map { "\($0.key)=\($0.value)" } == rhs.extra.map { "\($0.key)=\($0.value)" }
    }

    var entries: [(key: String, value: String)] {
        var list: [(key: String, value: String)] = []
        if enableUART { list.append(("enable_uart", "1")) }
        if uart2ndStage { list.append(("uart_2ndstage", "1")) }
        list += extra
        return list
    }
}

/// Renders the card's config.txt: the upstream rpi5-uefi file verbatim,
/// followed by one MaryPi block that only adds keys upstream did not set.
public enum ConfigTxt {
    public static let markerPrefix = "# ---- MaryPi"
    public static let markerSuffix = "/ravynOS (generated; the lines above are upstream rpi5-uefi) ----"

    public static func marker(version: String) -> String {
        "\(markerPrefix) \(version)\(markerSuffix)"
    }

    /// Keys assigned anywhere in the text (`key=value`, ignoring comments and
    /// `[section]` filters).
    public static func keys(in text: String) -> Set<String> {
        var keys = Set<String>()
        for rawLine in text.split(omittingEmptySubsequences: false, whereSeparator: \.isNewline) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            guard !line.isEmpty, !line.hasPrefix("#"), !line.hasPrefix("[") else { continue }
            guard let equals = line.firstIndex(of: "=") else { continue }
            keys.insert(line[..<equals].trimmingCharacters(in: .whitespaces))
        }
        return keys
    }

    /// Strip a previously generated MaryPi block so rendering is idempotent.
    public static func upstreamPortion(of text: String) -> String {
        var upstream = text
        if let range = text.range(of: markerPrefix) {
            upstream = String(text[..<range.lowerBound])
        }
        while upstream.hasSuffix("\n") || upstream.hasSuffix("\r") {
            upstream.removeLast()
        }
        return upstream
    }

    public static func render(upstream: String, marypiVersion: String, config: RavynBootConfig = RavynBootConfig()) -> String {
        let base = upstreamPortion(of: upstream) + "\n"
        let existing = keys(in: base)

        var block = "\n\(marker(version: marypiVersion))\n"
        block += "# Serial console on the 3-pin debug UART (115200 8N1). The UEFI firmware\n"
        block += "# and the ravynOS kernel both log here.\n"
        for (key, value) in config.entries {
            if existing.contains(key) {
                block += "# \(key) already set above; left unchanged\n"
            } else {
                block += "\(key)=\(value)\n"
            }
        }
        return base + block
    }
}
