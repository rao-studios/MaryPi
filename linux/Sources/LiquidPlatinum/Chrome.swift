import SwiftUI

/// The 42 pt title bar: the title-bar surface with its sheen, the traffic lights 12 pt in, the title centred
/// over the whole bar between 98 pt insets, and a hairline under it.
public struct LPTitleBar<Accessory: View>: View {
    var title: String
    var sheen: CGFloat
    var zoomed: Bool
    var onClose: () -> Void
    var onMinimize: () -> Void
    var onZoom: () -> Void
    var accessory: Accessory
    @Environment(\.lpWindowActive) private var active

    public init(_ title: String, sheen: CGFloat = 0.35, zoomed: Bool = false, onClose: @escaping () -> Void = {},
                onMinimize: @escaping () -> Void = {}, onZoom: @escaping () -> Void = {}, @ViewBuilder accessory: () -> Accessory) {
        self.title = title
        self.sheen = sheen
        self.zoomed = zoomed
        self.onClose = onClose
        self.onMinimize = onMinimize
        self.onZoom = onZoom
        self.accessory = accessory()
    }

    /// Both edges of the title keep clear of three beads, two gaps and `space.6`.
    public static var titleInset: CGFloat { 3 * LP.Size.traffic + 2 * LP.Size.trafficGap + LP.Space._6 }

    public var body: some View {
        ZStack {
            BrushedSurface(.titlebar(active: active), in: UnevenRoundedRectangle(topLeadingRadius: LP.Radius.window, topTrailingRadius: LP.Radius.window),
                           sheen: sheen)
            Text(title)
                .font(.system(size: LP.Text.lg, weight: .semibold))
                .foregroundStyle((active ? LP.Ink.primary : LP.Ink.tertiary).color)
                .lpEmbossed()
                .lineLimit(1)
                .truncationMode(.tail)
                .padding(.horizontal, Self.titleInset)
            HStack(spacing: 0) {
                TrafficLights(zoomed: zoomed, onClose: onClose, onMinimize: onMinimize, onZoom: onZoom)
                Spacer(minLength: 0)
                accessory
            }
            .padding(.horizontal, LP.Space._3)
        }
        .frame(height: LP.Size.titlebarHeight)
        .overlay(alignment: .bottom) {
            Rectangle().fill(LP.Edge.hairline.color).frame(height: 1).offset(y: 1)
        }
        .zIndex(1)
    }
}

/// A 40 pt toolbar: flat metal, an extra light line inside its top, a hairline under it.
public struct LPToolbar<Content: View>: View {
    var content: Content

    public init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }

    public var body: some View {
        HStack(spacing: LP.Space._2) { content }
            .padding(.horizontal, LP.Space._3)
            .frame(maxWidth: .infinity, minHeight: 40, maxHeight: 40)
            .background {
                ZStack {
                    BrushedSurface(.flat)
                    InsetShadow(Rectangle(), color: LP.Edge.light, y: 1)
                }
            }
            .overlay(alignment: .bottom) {
                Rectangle().fill(LP.Edge.hairline.color).frame(height: 1).offset(y: 1)
            }
            .zIndex(1)
    }
}

/// A 22 pt status bar: a platinum gradient under a divider, 11 pt secondary text.
public struct LPStatusBar<Content: View>: View {
    var content: Content

    public init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }

    public var body: some View {
        HStack(spacing: LP.Space._3) { content }
            .font(.system(size: LP.Text.xs).monospacedDigit())
            .foregroundStyle(LP.Ink.secondary.color)
            .padding(.horizontal, LP.Space._3)
            .frame(maxWidth: .infinity, minHeight: 22, maxHeight: 22)
            .background {
                ZStack {
                    LinearGradient(colors: [LP.Platinum._2.color, LP.Platinum._3.color], startPoint: .top, endPoint: .bottom)
                    InsetShadow(Rectangle(), color: LP.Edge.light, y: 1)
                }
            }
            .overlay(alignment: .top) {
                Rectangle().fill(LP.Edge.divider.color).frame(height: 1)
            }
    }
}

/// A 180 pt sidebar: flat `surface.sidebar`, no grain, a divider inside its right edge.
public struct LPSidebar<Content: View>: View {
    var content: Content

    public init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 0) { content }
            .padding(LP.Space._2)
            .frame(width: 180)
            .frame(maxHeight: .infinity, alignment: .top)
            .background(LP.Surface.sidebar.color)
            .overlay(alignment: .trailing) {
                Rectangle().fill(LP.Edge.divider.color).frame(width: 1)
            }
    }
}

public struct LPSidebarHeader: View {
    var title: String

    public init(_ title: String) {
        self.title = title
    }

    public var body: some View {
        Text(title.uppercased())
            .font(.system(size: LP.Text.xs, weight: .semibold))
            .tracking(0.04 * LP.Text.xs)
            .foregroundStyle(LP.Ink.tertiary.color)
            .padding(.horizontal, LP.Space._2)
            .frame(maxWidth: .infinity, minHeight: 16, maxHeight: 16, alignment: .leading)
            .padding(.bottom, LP.Space._1)
    }
}

/// A 24 pt sidebar row; selected, it takes the accent's gradient and on-accent ink.
public struct LPSidebarRow<Accessory: View>: View {
    var symbol: String
    var title: String
    var selected: Bool
    var accessory: Accessory
    @Environment(\.lpAccent) private var accent
    @State private var hovering = false

    public init(symbol: String, title: String, selected: Bool, @ViewBuilder accessory: () -> Accessory) {
        self.symbol = symbol
        self.title = title
        self.selected = selected
        self.accessory = accessory()
    }

    public var body: some View {
        HStack(spacing: LP.Space._2) {
            Image(systemName: symbol)
                .font(.system(size: 13))
                .frame(width: 15)
                .foregroundStyle((selected ? LP.Ink.onAccent : accent.base).color)
            Text(title)
                .font(.system(size: LP.Text.md))
                .foregroundStyle((selected ? LP.Ink.onAccent : LP.Ink.primary).color)
                .lineLimit(1)
            Spacer(minLength: 0)
            accessory
        }
        .modifier(OnAccentShadow(active: selected))
        .padding(.horizontal, LP.Space._2)
        .frame(height: 24)
        .background {
            let shape = RoundedRectangle(cornerRadius: LP.Radius.sm, style: .continuous)
            if selected {
                ZStack {
                    shape.fill(LinearGradient(colors: [accent.light.color, accent.base.color], startPoint: .top, endPoint: .bottom))
                    InsetShadow(shape, color: LPColor.white.opacity(0.35), y: 1)
                }
            } else if hovering {
                shape.fill(Color.black.opacity(0.05))
            }
        }
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
    }
}

private struct OnAccentShadow: ViewModifier {
    var active: Bool

    func body(content: Content) -> some View {
        if active { content.lpOnAccent() } else { content }
    }
}

/// An empty state: 13 pt tertiary text, 24 pt under the top of the content.
public struct LPEmptyState: View {
    var symbol: String?
    var title: String
    var message: String?

    public init(symbol: String? = nil, title: String, message: String? = nil) {
        self.symbol = symbol
        self.title = title
        self.message = message
    }

    public var body: some View {
        VStack(spacing: LP.Space._2) {
            if let symbol {
                Image(systemName: symbol)
                    .font(.system(size: 40, weight: .light))
                    .foregroundStyle(LP.Ink.disabled.color)
                    .padding(.bottom, LP.Space._2)
            }
            Text(title)
                .font(.system(size: LP.Text.md, weight: .semibold))
                .foregroundStyle(LP.Ink.secondary.color)
            if let message {
                Text(message)
                    .font(.system(size: LP.Text.md))
                    .foregroundStyle(LP.Ink.tertiary.color)
                    .multilineTextAlignment(.center)
            }
        }
        .lpEmbossed()
        .padding(LP.Space._6)
    }
}

extension LPTitleBar where Accessory == EmptyView {
    public init(_ title: String, sheen: CGFloat = 0.35, zoomed: Bool = false, onClose: @escaping () -> Void = {},
                onMinimize: @escaping () -> Void = {}, onZoom: @escaping () -> Void = {}) {
        self.init(title, sheen: sheen, zoomed: zoomed, onClose: onClose, onMinimize: onMinimize, onZoom: onZoom) { EmptyView() }
    }
}

extension LPSidebarRow where Accessory == EmptyView {
    public init(symbol: String, title: String, selected: Bool) {
        self.init(symbol: symbol, title: title, selected: selected) { EmptyView() }
    }
}
