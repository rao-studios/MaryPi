import SwiftUI

/// Every piece in one small window, for reviewing Liquid Platinum on the Mac (and for the render test).
public struct LiquidPlatinumGallery: View {
    @State private var quality = 0
    @State private var host = ""
    var showSheet: Bool
    var active: Bool

    public init(showSheet: Bool = false, active: Bool = true) {
        self.showSheet = showSheet
        self.active = active
    }

    public var body: some View {
        VStack(spacing: 0) {
            LPTitleBar("MaryVNC — maryos", sheen: 0.4)
            VStack(spacing: 0) {
                LPToolbar {
                    Button("Connect") {}.buttonStyle(LPPillButtonStyle())
                    Button("Disconnect") {}.buttonStyle(LPPillButtonStyle()).disabled(true)
                    LPSegmented(selection: $quality, options: [(0, "Best"), (1, "Fast")], small: true)
                    Spacer()
                    Button("Pair over USB") {}.buttonStyle(LPPillButtonStyle(.primary))
                }
                HStack(spacing: 0) {
                    LPSidebar {
                        LPSidebarHeader("On the cable")
                        LPSidebarRow(symbol: "cable.connector", title: "maryos", selected: true)
                        LPSidebarHeader("Nearby").padding(.top, LP.Space._3)
                        LPSidebarRow(symbol: "desktopcomputer", title: "kitchen-pi", selected: false)
                        Spacer()
                        // A static well: ImageRenderer cannot draw AppKit's text field.
                        LPWell { Text("maryos.local") }
                    }
                    ZStack {
                        BrushedSurface(.body)
                        LPEmptyState(symbol: "display", title: "No Pi connected", message: "Plug a Pi in over USB or join its Wi-Fi.")
                    }
                }
                LPStatusBar { Text("Connected over USB · 1280×800 · 14 fps") }
            }
            .overlay {
                if showSheet {
                    LPSheet(symbol: "cable.connector", title: "Pair with “maryos”?",
                            message: "This Mac and the Pi will remember each other. Check that the Pi shows 77f9 72d5 895b 8508.",
                            actions: [.init("Pair", role: .primary) {}, .init("Cancel", role: .cancel) {}])
                }
            }
        }
        .frame(width: 720, height: 460)
        .background(LP.Surface.windowBottom.color)
        .clipShape(RoundedRectangle(cornerRadius: LP.Radius.window, style: .continuous))
        .coordinateSpace(.named(lpWindowSpace))
        .environment(\.lpWindowActive, active)
    }
}
