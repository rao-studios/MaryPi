import Darwin
import Foundation
import Synchronization

/// What one answer said: where it came from, the paired Pi whose tag it carried, and a Pi's offer to pair.
public struct NearbyHeard: Equatable, Sendable {
    /// The address the answer came from; an IPv6 link-local one ends in %interface.
    public let host: String
    /// maryvncd's TCP port.
    public let port: UInt16
    /// The paired Pi whose tag the answer carried: proven to be that Pi (its address is not).
    public let piPublicKey: [UInt8]?
    /// The Pi's key and name, while its pairing window is open.
    public let offer: NearbyAnswer.Offer?
    /// The number of the call it answered: `call` numbers them from 1, in the order they are made.
    public let call: Int
}

/// A Pi in view, as the sidebar lists it.
public struct NearbyPi: Identifiable, Equatable, Sendable {
    public let publicKey: [UInt8]
    public var name: String
    public var host: String
    public var port: UInt16
    /// Its answer carried this Mac's tag.
    public var isPaired: Bool
    /// Its pairing window is open.
    public var isPairable: Bool
    public var lastSeen: Date

    public init(publicKey: [UInt8], name: String, host: String, port: UInt16, isPaired: Bool, isPairable: Bool, lastSeen: Date) {
        self.publicKey = publicKey
        self.name = name
        self.host = host
        self.port = port
        self.isPaired = isPaired
        self.isPairable = isPairable
        self.lastSeen = lastSeen
    }

    public var fingerprint: Fingerprint { Fingerprint(publicKey: publicKey) }
    public var id: String { fingerprint.hex }
}

/// Makes Nearby calls and hears the answers. A call goes to each address given (the paired Pis' last ones) and to
/// both groups on every interface that can carry them. Answers come back to the ephemeral port the call left from,
/// so the scanner never listens on a port of its own and the Mac's firewall only sees replies; only answers to
/// this scanner's calls from the last ten seconds count. BSD sockets, as a viewer on any system would use.
public final class NearbyScanner: @unchecked Sendable {
    public let heard: AsyncStream<NearbyHeard>
    /// Told, on the scanner's queue, when a call could not go out: the errno and where it was going. macOS says
    /// EHOSTUNREACH while the app has no Local Network access. Set it before `start()`.
    public var sendFailed: (@Sendable (_ error: Int32, _ destination: String) -> Void)?

    private let continuation: AsyncStream<NearbyHeard>.Continuation
    private let queue = DispatchQueue(label: "com.maryos.MaryVNC.nearby")
    private let port: UInt16
    private let numbers = Mutex(0)
    private var sources: [DispatchSourceRead] = []
    private var fd4: Int32 = -1
    private var fd6: Int32 = -1
    private var calls: [(challenge: [UInt8], keys: [NearbyKey], at: ContinuousClock.Instant, number: Int)] = []

    /// `port`: where Pis answer, 5901 but for tests.
    public init(port: UInt16 = Nearby.port) {
        self.port = port
        (heard, continuation) = AsyncStream.makeStream(of: NearbyHeard.self)
    }

    deinit {
        for source in sources { source.cancel() }
        continuation.finish()
    }

    /// Opens a UDP socket for IPv4 and one for IPv6; throws when neither opens.
    public func start() throws {
        try queue.sync {
            guard sources.isEmpty else { return }
            fd4 = Self.open(AF_INET)
            fd6 = Self.open(AF_INET6)
            guard fd4 >= 0 || fd6 >= 0 else { throw MaryVNCError("no UDP socket to call with: \(String(cString: strerror(errno)))") }
            for fd in [fd4, fd6] where fd >= 0 {
                let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
                source.setEventHandler { [weak self] in self?.receive(fd) }
                source.setCancelHandler { close(fd) }
                source.resume()
                sources.append(source)
            }
        }
    }

    public func stop() {
        queue.sync {
            for source in sources { source.cancel() }
            sources = []
            fd4 = -1
            fd6 = -1
        }
    }

    /// One call carrying a tag for each key (at most 32), to each address and, with `multicast`, to the groups.
    /// Returns the call's number, which the answers to it carry.
    @discardableResult
    public func call(keys: [NearbyKey], addresses: [String], multicast: Bool = true) -> Int {
        let number = numbers.withLock { value in
            value += 1
            return value
        }
        queue.async { [self] in
            let keys = Array(keys.prefix(Nearby.tagsMax))
            guard !sources.isEmpty, let call = try? NearbyCall(keys: keys), let bytes = try? call.encoded() else { return }
            let now = ContinuousClock.now
            calls.removeAll { now - $0.at > .seconds(10) }
            calls.append((call.challenge, keys, now, number))
            if calls.count > 16 { calls.removeFirst(calls.count - 16) }
            for address in Set(addresses) { send(bytes, to: address) }
            if multicast { sendToGroups(bytes) }
        }
        return number
    }

    // MARK: On the queue

    private static func open(_ family: Int32) -> Int32 {
        let fd = socket(family, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { return -1 }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        _ = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)
        _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
        let bound: Int32
        if family == AF_INET {
            var hops: UInt8 = 1, loop: UInt8 = 0
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &hops, 1)
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, 1)
            var address = sockaddr_in()
            address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            address.sin_family = sa_family_t(AF_INET)
            bound = withUnsafePointer(to: &address) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
            }
        } else {
            var hops: Int32 = 1, loop: UInt32 = 0
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, socklen_t(MemoryLayout<Int32>.size))
            setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, socklen_t(MemoryLayout<Int32>.size))
            setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &loop, socklen_t(MemoryLayout<UInt32>.size))
            var address = sockaddr_in6()
            address.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
            address.sin6_family = sa_family_t(AF_INET6)
            address.sin6_addr = in6addr_any
            bound = withUnsafePointer(to: &address) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in6>.size)) }
            }
        }
        guard bound == 0 else {
            close(fd)
            return -1
        }
        return fd
    }

    private struct Interface {
        let name: String
        let index: UInt32
        var ipv4: in_addr?
        var ipv6 = false
    }

    /// The interfaces a call goes out on: up, multicast, not loopback, and not one of macOS's own point-to-point or
    /// peer-to-peer links.
    private static func interfaces() -> [Interface] {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return [] }
        defer { freeifaddrs(head) }
        let skipped = ["awdl", "llw", "utun", "ipsec", "anpi", "gif", "stf", "bridge", "ap"]
        var byName: [String: Interface] = [:]
        for entry in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let flags = entry.pointee.ifa_flags
            guard let address = entry.pointee.ifa_addr, flags & UInt32(IFF_UP) != 0, flags & UInt32(IFF_MULTICAST) != 0,
                  flags & UInt32(IFF_LOOPBACK) == 0 else { continue }
            let name = String(cString: entry.pointee.ifa_name)
            guard !skipped.contains(where: { name.hasPrefix($0) }) else { continue }
            let family = Int32(address.pointee.sa_family)
            guard family == AF_INET || family == AF_INET6 else { continue }
            var interface = byName[name] ?? Interface(name: name, index: if_nametoindex(name))
            if family == AF_INET {
                interface.ipv4 = address.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee.sin_addr }
            } else {
                interface.ipv6 = true
            }
            byName[name] = interface
        }
        return byName.values.sorted { $0.name < $1.name }
    }

    private func sendToGroups(_ bytes: [UInt8]) {
        for interface in Self.interfaces() {
            if fd4 >= 0, var address = interface.ipv4 {
                setsockopt(fd4, IPPROTO_IP, IP_MULTICAST_IF, &address, socklen_t(MemoryLayout<in_addr>.size))
                var group = sockaddr_in()
                group.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
                group.sin_family = sa_family_t(AF_INET)
                group.sin_port = port.bigEndian
                inet_pton(AF_INET, Nearby.group4, &group.sin_addr)
                send(bytes, on: fd4, to: &group, named: "\(Nearby.group4) on \(interface.name)")
            }
            if fd6 >= 0, interface.ipv6, interface.index != 0 {
                var index = interface.index
                setsockopt(fd6, IPPROTO_IPV6, IPV6_MULTICAST_IF, &index, socklen_t(MemoryLayout<UInt32>.size))
                var group = sockaddr_in6()
                group.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
                group.sin6_family = sa_family_t(AF_INET6)
                group.sin6_port = port.bigEndian
                group.sin6_scope_id = index
                inet_pton(AF_INET6, Nearby.group6, &group.sin6_addr)
                send(bytes, on: fd6, to: &group, named: "\(Nearby.group6) on \(interface.name)")
            }
        }
    }

    private func send<Address>(_ bytes: [UInt8], on fd: Int32, to address: inout Address, named destination: String) {
        let length = socklen_t(MemoryLayout<Address>.size)
        let sent = bytes.withUnsafeBytes { buffer in
            withUnsafePointer(to: &address) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sendto(fd, buffer.baseAddress, buffer.count, 0, $0, length) }
            }
        }
        if sent < 0 { failed(errno, destination) }
    }

    /// A call to one address, when it is a numeric one (a name would need a lookup).
    private func send(_ bytes: [UInt8], to host: String) {
        var hints = addrinfo()
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV
        hints.ai_socktype = SOCK_DGRAM
        var result: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, String(port), &hints, &result) == 0, let info = result else { return }
        defer { freeaddrinfo(result) }
        let fd = info.pointee.ai_family == AF_INET6 ? fd6 : fd4
        guard fd >= 0, let address = info.pointee.ai_addr else { return }
        let sent = bytes.withUnsafeBytes { sendto(fd, $0.baseAddress, $0.count, 0, address, info.pointee.ai_addrlen) }
        if sent < 0 { failed(errno, host) }
    }

    private func failed(_ error: Int32, _ destination: String) {
        MaryVNC.logger.debug("a Nearby call to \(destination, privacy: .public) did not go: \(String(cString: strerror(error)), privacy: .public)")
        sendFailed?(error, destination)
    }

    private func receive(_ fd: Int32) {
        var buffer = [UInt8](repeating: 0, count: Nearby.datagramMax + 1)
        while true {
            var storage = sockaddr_storage()
            var length = socklen_t(MemoryLayout<sockaddr_storage>.size)
            let count = buffer.withUnsafeMutableBytes { bytes in
                withUnsafeMutablePointer(to: &storage) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, bytes.baseAddress, bytes.count, 0, $0, &length) }
                }
            }
            guard count > 0 else { return }
            guard count <= Nearby.datagramMax else { continue }
            hear(Array(buffer[..<count]), from: &storage, length: length)
        }
    }

    private func hear(_ bytes: [UInt8], from storage: inout sockaddr_storage, length: socklen_t) {
        guard let answer = try? NearbyAnswer(decoding: bytes) else { return }
        let now = ContinuousClock.now
        calls.removeAll { now - $0.at > .seconds(10) }
        guard let call = calls.first(where: { $0.challenge == answer.challenge }) else { return }
        let key = call.keys.first { answer.isTagged(for: $0) }
        guard key != nil || answer.offer != nil else { return }
        var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
        let named = withUnsafePointer(to: &storage) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getnameinfo($0, length, &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) }
        }
        guard named == 0 else { return }
        let text = String(decoding: host.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
        continuation.yield(NearbyHeard(host: text, port: answer.port, piPublicKey: key?.piPublicKey, offer: answer.offer, call: call.number))
    }
}
