import Darwin
import Foundation
import Network

/// A Pi announcing `_maryvnc._tcp`: its host name, the fingerprint its TXT record claims (the handshake
/// proves the key), and whether it was heard over the USB cable.
public struct DiscoveredPi: Hashable, Sendable, Identifiable {
    public let name: String
    public let type: String
    public let domain: String
    public let fingerprint: Fingerprint?
    public let version: Int?
    public let interfaceNames: [String]
    public let isUSB: Bool

    public var id: String { fingerprint?.hex ?? name }

    public init(name: String, type: String = MaryVNC.serviceType, domain: String = "local.", fingerprint: Fingerprint?,
                version: Int?, interfaceNames: [String], isUSB: Bool) {
        self.name = name
        self.type = type
        self.domain = domain
        self.fingerprint = fingerprint
        self.version = version
        self.interfaceNames = interfaceNames
        self.isUSB = isUSB
    }

    public var endpoint: NWEndpoint { .service(name: name, type: type, domain: domain, interface: nil) }

    init?(result: NWBrowser.Result, usbInterfaces: Set<String>) {
        guard case let .service(name, type, domain, _) = result.endpoint else { return nil }
        var txt: [String: String] = [:]
        if case let .bonjour(record) = result.metadata { txt = record.dictionary }
        let parsed = DiscoveredPi.parseTXT(txt)
        let names = result.interfaces.map(\.name)
        self.init(name: name, type: type, domain: domain, fingerprint: parsed.fingerprint, version: parsed.version,
                  interfaceNames: names, isUSB: !usbInterfaces.isDisjoint(with: names))
    }

    /// `id=<64 hex> v=1`, as `maryvncd init` writes the record.
    static func parseTXT(_ txt: [String: String]) -> (fingerprint: Fingerprint?, version: Int?) {
        (txt["id"].flatMap(Fingerprint.init(hex:)), txt["v"].flatMap { Int($0) })
    }
}

/// The USB cable's network (chapter 13 in MaryOS): the Pi is 10.12.194.1/28 and hands the Mac an address
/// in the same /28. A Bonjour result heard on an interface where this Mac has such an address came over
/// the cable. Nothing is sent to find out.
public enum USBLink {
    public static func contains(_ address: UInt32) -> Bool {
        address & 0xffff_fff0 == 0x0a0c_c200
    }

    /// The names of this Mac's interfaces with an address on the cable's network.
    public static func interfaces() -> Set<String> {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return [] }
        defer { freeifaddrs(head) }
        var names: Set<String> = []
        for entry in sequence(first: first, next: { $0.pointee.ifa_next }) {
            guard let address = entry.pointee.ifa_addr, address.pointee.sa_family == UInt8(AF_INET) else { continue }
            let raw = address.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee.sin_addr.s_addr }
            if contains(UInt32(bigEndian: raw)) { names.insert(String(cString: entry.pointee.ifa_name)) }
        }
        return names
    }
}

/// Browses for Pis. Each value is every Pi in view, USB first and then by name.
public final class PiBrowser: Sendable {
    private let queue = DispatchQueue(label: "com.maryos.MaryVNC.browser")

    public init() {}

    public func results() -> AsyncStream<[DiscoveredPi]> {
        AsyncStream { continuation in
            let browser = NWBrowser(for: .bonjourWithTXTRecord(type: MaryVNC.serviceType, domain: nil), using: .tcp)
            browser.browseResultsChangedHandler = { results, _ in
                let usb = USBLink.interfaces()
                let pis = results.compactMap { DiscoveredPi(result: $0, usbInterfaces: usb) }
                    .sorted { ($0.isUSB ? 0 : 1, $0.name) < ($1.isUSB ? 0 : 1, $1.name) }
                continuation.yield(pis)
            }
            browser.stateUpdateHandler = { state in
                switch state {
                case let .failed(error):
                    MaryVNC.logger.error("browsing for Pis failed: \(error.localizedDescription, privacy: .public)")
                    continuation.finish()
                case .cancelled:
                    continuation.finish()
                default:
                    break
                }
            }
            let handle = BrowserHandle(browser)
            continuation.onTermination = { _ in handle.browser.cancel() }
            browser.start(queue: queue)
        }
    }
}

/// NWBrowser is used from its own queue only; this carries it into the termination handler.
private final class BrowserHandle: @unchecked Sendable {
    let browser: NWBrowser

    init(_ browser: NWBrowser) {
        self.browser = browser
    }
}
