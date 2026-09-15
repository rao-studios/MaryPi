import CoreGraphics
import Network

/// Where a session goes, and with which key.
public struct SessionTarget: Equatable {
    public var name: String
    public var endpoint: NWEndpoint
    /// The address to remember for the next Nearby call.
    public var host: String?
    /// The Pi's key to resume with; nil pairs.
    public var piKey: [UInt8]?
    /// The Pi's fingerprint: the one to pair with, or the one resumed with.
    public var fingerprint: Fingerprint?

    public init(name: String, endpoint: NWEndpoint, host: String?, piKey: [UInt8]?, fingerprint: Fingerprint?) {
        self.name = name
        self.endpoint = endpoint
        self.host = host
        self.piKey = piKey
        self.fingerprint = fingerprint
    }

    /// A Pi in view: resumed with `key`, or without one paired with the key its answer offered.
    public init(pi: NearbyPi, key: [UInt8]?) {
        self.init(name: pi.name, endpoint: pi.endpoint, host: pi.host, piKey: key, fingerprint: pi.fingerprint)
    }
}

/// What a viewer does when a session ends: the rules MaryVNC and MaryVNC Light share.
public enum SessionOutcome: Equatable, Sendable {
    /// This viewer ended it.
    case closed
    /// Stay disconnected, and say why.
    case settle(title: String, text: String)
    /// The Pi proved its key and said it has forgotten this Mac: forget the Pi here too, and say so.
    case forget(title: String, text: String)
    /// The link went: try again.
    case retry
}

extension SessionEnd {
    /// `name`: the Pi's, as the messages call it.
    public func outcome(name: String) -> SessionOutcome {
        switch self {
        case .byViewer:
            .closed
        case .bye("replaced"):
            .settle(title: "Another Mac is watching \(name)", text: "A Pi shows its desktop to one Mac at a time. Connect again to take it back.")
        case .bye("forgotten"):
            .forget(title: "\(name) no longer knows this Mac", text: "Pair again: press the Pi’s power button, then click Pair.")
        case .notPaired:
            // A silent refusal proves nothing about who refused: an address can be stale, or another machine's. So the
            // pairing stays, and Forget This Pi… is there if the Pi really has forgotten this Mac.
            .settle(title: "\(name) did not accept this Mac",
                    text: "Either the Pi has forgotten this Mac, or another machine answered at that address. If maryvncctl pairs on the Pi no longer lists this Mac, choose Forget This Pi… and pair again.")
        case .pairingRefused:
            .settle(title: "\(name) did not accept pairing",
                    text: "Its pairing window is closed: it closes after two minutes, and as soon as a Mac pairs or connects. Press the Pi’s power button (or run maryvncctl pair-window on it), then click Pair again.")
        case .wrongPi:
            .settle(title: "That Pi proved another key", text: "The key it proved is not the one its answer carried, so MaryVNC left before sending this Mac’s key.")
        case .bye, .failed:
            .retry
        }
    }
}

/// A CGImage handed from the decoder to a window.
public struct DecodedFrame: @unchecked Sendable {
    public let image: CGImage
}

/// Applies frames off the main thread; the framebuffer lives here and nowhere else.
public actor FrameDecoder {
    private var framebuffer: Framebuffer?

    public init() {}

    public func resize(width: Int, height: Int) {
        if framebuffer?.width != width || framebuffer?.height != height {
            framebuffer = try? Framebuffer(width: width, height: height)
        }
    }

    public func apply(_ rects: [FrameRect]) -> DecodedFrame? {
        guard let framebuffer else { return nil }
        for rect in rects {
            try? framebuffer.apply(rect)
        }
        return framebuffer.makeImage().map(DecodedFrame.init)
    }
}
