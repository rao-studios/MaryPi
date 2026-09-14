import LiquidPlatinum
import MaryVNCKit
import SwiftUI

/// The viewer's one window: title bar, toolbar, the Pis on the left, the desktop, the status bar.
struct ViewerWindow: View {
    @Environment(AppModel.self) private var model
    @State private var window = WindowState()

    var body: some View {
        VStack(spacing: 0) {
            LPTitleBar(model.windowTitle, sheen: window.sheen, zoomed: window.isZoomed,
                       onClose: { window.close() }, onMinimize: { window.minimize() }, onZoom: { window.zoom() })
                .gesture(WindowDragGesture())
                .allowsWindowActivationEvents(true)
                .onTapGesture(count: 2) { window.zoom() }
            VStack(spacing: 0) {
                toolbar
                HStack(spacing: 0) {
                    sidebar
                    content
                }
                LPStatusBar {
                    Text(model.statusText).lineLimit(1)
                }
            }
            .overlay { sheet }
        }
        .background(LP.Surface.windowBottom.color)
        .coordinateSpace(.named(lpWindowSpace))
        .environment(\.lpGrainOrigin, window.origin)
        .environment(\.lpWindowActive, window.isKey)
        .environment(\.lpAccent, model.settings.accent == .graphite ? .graphite : .blue)
        .background(WindowAccessor { window.attach($0) })
        .ignoresSafeArea()
        .frame(minWidth: 640, minHeight: 420)
    }

    private var toolbar: some View {
        LPToolbar {
            if model.isSessionActive {
                Button("Disconnect") { model.disconnect() }
                    .buttonStyle(LPPillButtonStyle())
            } else {
                Button("Connect") { model.connectSelected() }
                    .buttonStyle(LPPillButtonStyle())
                    .disabled(model.selectedPi == nil)
            }
            Button("Refresh") { model.refresh() }
                .buttonStyle(LPPillButtonStyle())
                .disabled(!model.isConnected)
            LPSegmented(selection: Binding(get: { model.settings.quality }, set: { model.setQuality($0) }),
                        options: [(FrameQuality.best, "Best"), (FrameQuality.fast, "Fast")], small: true)
            Spacer(minLength: LP.Space._2)
            Button("Connect to Address…") { model.sheet = .connect }
                .buttonStyle(LPPillButtonStyle())
            Button("Pair over USB") { model.pairSelected() }
                .buttonStyle(LPPillButtonStyle(.primary))
                .disabled(!model.canPairSelected)
        }
    }

    private var sidebar: some View {
        let usb = model.pis.filter(\.isUSB), nearby = model.pis.filter { !$0.isUSB }, away = model.pairedOutOfView
        return LPSidebar {
            if !usb.isEmpty {
                LPSidebarHeader("On the cable")
                ForEach(usb) { row($0) }
            }
            if !nearby.isEmpty {
                LPSidebarHeader("Nearby").padding(.top, usb.isEmpty ? 0 : LP.Space._3)
                ForEach(nearby) { row($0) }
            }
            if !away.isEmpty {
                LPSidebarHeader("Paired, not in view").padding(.top, model.pis.isEmpty ? 0 : LP.Space._3)
                ForEach(away) { paired in
                    let current = model.isCurrent(paired)
                    LPSidebarRow(symbol: "desktopcomputer", title: paired.name, selected: false) {
                        if current { statusDot }
                    }
                    .opacity(current ? 1 : 0.55)
                }
            }
            if model.pis.isEmpty && away.isEmpty {
                Text("Looking for Pis…")
                    .font(.system(size: LP.Text.sm))
                    .foregroundStyle(LP.Ink.tertiary.color)
                    .padding(.horizontal, LP.Space._2)
            }
            Spacer(minLength: LP.Space._3)
            VStack(alignment: .leading, spacing: 2) {
                Text("This Mac")
                    .font(.system(size: LP.Text.xs, weight: .semibold))
                Text(model.fingerprint.short)
                    .font(.system(size: LP.Text.xs).monospaced())
            }
            .foregroundStyle(LP.Ink.tertiary.color)
            .padding(.horizontal, LP.Space._2)
            .help("This Mac’s key: \(model.fingerprint.display)")
        }
    }

    private func row(_ pi: DiscoveredPi) -> some View {
        let selected = model.selectionID == pi.id
        return LPSidebarRow(symbol: pi.isUSB ? "cable.connector" : "desktopcomputer", title: pi.name, selected: selected) {
            if model.isCurrent(pi) {
                statusDot
            } else if !model.isPaired(pi) {
                Text("Pair")
                    .font(.system(size: LP.Text.xs, weight: .medium))
                    .foregroundStyle((selected ? LP.Ink.onAccent : LP.Ink.tertiary).color)
            }
        }
        .onTapGesture(count: 2) { model.select(pi); model.connectSelected() }
        .onTapGesture { model.select(pi) }
    }

    /// Green while connected, amber while connecting or waiting to reconnect.
    private var statusDot: some View {
        Circle()
            .fill(model.isConnected ? LP.Traffic.Zoom.base.color : LP.Traffic.Minimize.base.color)
            .frame(width: 7, height: 7)
    }

    @ViewBuilder private var content: some View {
        ZStack {
            if let frame = model.frame {
                LP.Platinum._9.color
                RemoteView(image: frame, desktopSize: model.desktopSize, cursorShown: model.cursorShown,
                           keyMap: KeyMap(commandKey: model.settings.commandKey), enabled: model.isConnected,
                           send: { model.send($0) })
                    .opacity(model.isConnected ? 1 : 0.45)
            } else {
                BrushedSurface(.body)
                let empty = model.emptyState
                LPEmptyState(symbol: empty.symbol, title: empty.title, message: empty.message)
            }
        }
    }

    @ViewBuilder private var sheet: some View {
        switch model.sheet {
        case let .pair(pi):
            LPSheet(symbol: "cable.connector", title: "Pair with “\(pi.name)”?",
                    message: "This Mac and the Pi will remember each other: over the cable now, and on the network after. The Pi announces the key \(pi.fingerprint?.display ?? "(none)"); `maryvncctl status` on the Pi shows its own.",
                    actions: [.init("Pair", role: .primary) { model.confirmPair(pi) }, .init("Cancel", role: .cancel) { model.sheet = nil }])
        case .connect:
            ConnectSheet()
        case let .message(title, text):
            LPSheet(symbol: "exclamationmark.triangle", title: title, message: text,
                    actions: [.init("OK", role: .primary) { model.sheet = nil }])
        case let .forget(paired):
            LPSheet(symbol: "trash", title: "Forget “\(paired.name)”?",
                    message: "This Mac will have to pair with it over the USB cable again. On the Pi, `maryvncctl forget` removes this Mac as well.",
                    actions: [.init("Forget", role: .primary) { model.forget(paired) }, .init("Cancel", role: .cancel) { model.sheet = nil }])
        case nil:
            EmptyView()
        }
    }
}

/// Connect to a Pi by address: resume with the most recent Pi's key, or pair when its window is open.
struct ConnectSheet: View {
    @Environment(AppModel.self) private var model
    @State private var address = ""
    @State private var pair = false

    var body: some View {
        LPSheet(symbol: "network", title: "Connect to a Pi by address",
                message: "A name such as maryos.local or an address such as 10.0.0.72, with :port if it is not 5901.",
                actions: [.init("Connect", role: .primary) { submit() }, .init("Cancel", role: .cancel) { model.sheet = nil }]) {
            LPWellField("maryos.local", text: $address)
                .onSubmit { submit() }
            Toggle("Pair with it (its pairing window is open)", isOn: $pair)
                .toggleStyle(.checkbox)
                .font(.system(size: LP.Text.sm))
                .foregroundStyle(LP.Ink.secondary.color)
        }
    }

    private func submit() {
        let value = address.trimmingCharacters(in: .whitespaces)
        guard !value.isEmpty else { return }
        var host = value, port = MaryVNC.port
        if let colon = value.lastIndex(of: ":"), value.firstIndex(of: ":") == colon, let parsed = UInt16(value[value.index(after: colon)...]) {
            host = String(value[..<colon])
            port = parsed
        }
        model.connectManually(host: host, port: port, pair: pair)
    }
}

struct SettingsView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        VStack(alignment: .leading, spacing: LP.Space._4) {
            setting("Accent") {
                LPSegmented(selection: Binding(get: { model.settings.accent }, set: { model.setAccent($0) }),
                            options: [(ViewerSettings.Accent.blue, "Blue"), (ViewerSettings.Accent.graphite, "Graphite")])
            }
            setting("⌘ on the Pi") {
                LPSegmented(selection: Binding(get: { model.settings.commandKey }, set: { model.setCommandKey($0) }),
                            options: [(CommandKey.super, "Super"), (CommandKey.control, "Control")])
            }
            Text("Super keeps MaryOS’s own chords (⌘Space opens Spotlight, ⌘W closes a window). Control suits terminal programs such as foot. ⌘Q, ⌘H, ⌘M and ⌘, always stay with this Mac.")
                .font(.system(size: LP.Text.xs))
                .foregroundStyle(LP.Ink.tertiary.color)
                .fixedSize(horizontal: false, vertical: true)
            setting("Quality") {
                LPSegmented(selection: Binding(get: { model.settings.quality }, set: { model.setQuality($0) }),
                            options: [(FrameQuality.best, "Best"), (FrameQuality.fast, "Fast")])
            }
            Rectangle().fill(LP.Edge.divider.color).frame(height: 1)
            VStack(alignment: .leading, spacing: 2) {
                Text("This Mac’s key").font(.system(size: LP.Text.xs, weight: .semibold))
                Text(model.fingerprint.display).font(.system(size: LP.Text.xs).monospaced()).textSelection(.enabled)
            }
            .foregroundStyle(LP.Ink.secondary.color)
        }
        .padding(LP.Space._5)
        .frame(width: 440)
        .background(BrushedSurface(.flat))
        .coordinateSpace(.named(lpWindowSpace))
        .environment(\.lpAccent, model.settings.accent == .graphite ? .graphite : .blue)
    }

    private func setting(_ title: String, @ViewBuilder control: () -> some View) -> some View {
        HStack {
            Text(title)
                .font(.system(size: LP.Text.md, weight: .medium))
                .foregroundStyle(LP.Ink.primary.color)
                .lpEmbossed()
                .frame(width: 110, alignment: .leading)
            control()
            Spacer(minLength: 0)
        }
    }
}
