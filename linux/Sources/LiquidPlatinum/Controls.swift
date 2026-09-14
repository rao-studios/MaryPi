import SwiftUI

/// MaryUI's pill Button: 22 pt metal capsules with a hairline, a drop shadow and the raised emboss; primary
/// takes the accent's gradient.
public struct LPPillButtonStyle: ButtonStyle {
    public enum Kind: Sendable { case normal, primary }

    var kind: Kind
    var small: Bool

    public init(_ kind: Kind = .normal, small: Bool = false) {
        self.kind = kind
        self.small = small
    }

    public func makeBody(configuration: Configuration) -> some View {
        PillBody(label: configuration.label, pressed: configuration.isPressed, kind: kind, small: small)
    }
}

private struct PillBody<Label: View>: View {
    var label: Label
    var pressed: Bool
    var kind: LPPillButtonStyle.Kind
    var small: Bool
    @Environment(\.isEnabled) private var enabled
    @Environment(\.lpAccent) private var accent
    @State private var hovering = false

    private var gradient: [LPColor] {
        switch (kind, pressed, hovering && enabled) {
        case (.normal, true, _): [LP.Surface.pressedTop, LP.Surface.pressedBottom]
        case (.normal, false, true): [LP.Platinum._0, LP.Platinum._2]
        case (.normal, false, false): [LP.Surface.raisedTop, LP.Surface.raisedBottom]
        case (.primary, true, _): [accent.deep, accent.base]
        case (.primary, false, true): [accent.light, accent.deep]
        case (.primary, false, false): [accent.light, accent.base]
        }
    }

    var body: some View {
        let height: CGFloat = small ? LP.Size.controlHeightSm : LP.Size.controlHeight
        let shape = Capsule()
        label
            .font(.system(size: small ? LP.Text.sm : LP.Text.md, weight: .medium))
            .foregroundStyle((kind == .primary ? LP.Ink.onAccent : (enabled ? LP.Ink.primary : LP.Ink.disabled)).color)
            .modifier(PillInkShadow(primary: kind == .primary))
            .lineLimit(1)
            .padding(.horizontal, small ? LP.Space._2 : LP.Space._3)
            .frame(minWidth: height, minHeight: height, maxHeight: height)
            .background {
                ZStack {
                    shape.stroke(LP.Edge.hairline.color, lineWidth: 2)
                    shape.fill(LinearGradient(colors: gradient.map(\.color), startPoint: .top, endPoint: .bottom))
                        .shadow(color: Color.black.opacity(pressed ? 0 : 0.18), radius: 1, x: 0, y: 1)
                    BrushGrain(opacity: LP.Brush.opacity * 0.8).clipShape(shape)
                    if pressed {
                        InsetShadow(shape, color: LPColor.black.opacity(0.3), blur: 4, y: 2)
                    } else {
                        RaisedEmboss(shape)
                    }
                }
                .compositingGroup()
            }
            .offset(y: pressed ? 0.5 : 0)
            .opacity(enabled ? 1 : 0.7)
            .contentShape(shape)
            .onHover { hovering = $0 }
    }
}

private struct PillInkShadow: ViewModifier {
    var primary: Bool

    func body(content: Content) -> some View {
        if primary { content.shadow(color: Color.black.opacity(0.25), radius: 0, x: 0, y: 1) } else { content.lpEmbossed() }
    }
}

/// The recessed well MaryUI's TextField sits in: 22 pt, `surface.well` under the well emboss, a focus ring.
public struct LPWell<Content: View>: View {
    var focused: Bool
    var content: Content
    @Environment(\.lpAccent) private var accent

    public init(focused: Bool = false, @ViewBuilder content: () -> Content) {
        self.focused = focused
        self.content = content()
    }

    public var body: some View {
        let shape = RoundedRectangle(cornerRadius: LP.Radius.sm, style: .continuous)
        content
            .font(.system(size: LP.Text.md))
            .foregroundStyle(LP.Ink.primary.color)
            .padding(.horizontal, LP.Space._2)
            .frame(maxWidth: .infinity, minHeight: LP.Size.controlHeight, maxHeight: LP.Size.controlHeight, alignment: .leading)
            .background {
                ZStack {
                    shape.fill(LP.Surface.well.color)
                    WellEmboss(shape)
                }
            }
            .overlay {
                if focused {
                    RoundedRectangle(cornerRadius: LP.Radius.sm + 3, style: .continuous)
                        .stroke(accent.focusRing.color, lineWidth: 3)
                        .padding(-3)
                }
            }
    }
}

/// MaryUI's TextField: a well with the placeholder 4 pt clear of the caret (PARITY D36).
public struct LPWellField: View {
    var placeholder: String
    @Binding var text: String
    var monospaced: Bool
    @FocusState private var focused: Bool

    public init(_ placeholder: String, text: Binding<String>, monospaced: Bool = false) {
        self.placeholder = placeholder
        self._text = text
        self.monospaced = monospaced
    }

    public var body: some View {
        LPWell(focused: focused) {
            ZStack(alignment: .leading) {
                if text.isEmpty {
                    Text(placeholder)
                        .foregroundStyle(LP.Ink.tertiary.color)
                        .padding(.leading, 4)
                        .allowsHitTesting(false)
                }
                TextField("", text: $text)
                    .textFieldStyle(.plain)
                    .focused($focused)
            }
            .font(monospaced ? .system(size: LP.Text.md).monospaced() : .system(size: LP.Text.md))
        }
    }
}

/// MaryUI's SegmentedControl: equal segments in a recessed platinum track, a raised thumb on the choice.
public struct LPSegmented<Value: Hashable>: View {
    @Binding var selection: Value
    var options: [(value: Value, label: String)]
    var small: Bool
    @Namespace private var thumb
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    public init(selection: Binding<Value>, options: [(value: Value, label: String)], small: Bool = false) {
        self._selection = selection
        self.options = options
        self.small = small
    }

    public var body: some View {
        let height = small ? LP.Size.segmentedHeightSm : LP.Size.segmentedHeight
        let inner = height - 2 * LP.Size.segmentedPad
        let track = Capsule()
        EqualWidths {
            ForEach(options.indices, id: \.self) { index in
                let option = options[index]
                let chosen = option.value == selection
                Text(option.label)
                    .font(.system(size: small ? LP.Text.sm : LP.Text.md, weight: .medium))
                    .foregroundStyle((chosen ? LP.Ink.primary : LP.Ink.secondary).color)
                    .modifier(Embossed(active: chosen))
                    .lineLimit(1)
                    .padding(.horizontal, small ? LP.Space._3 : LP.Space._4)
                    .frame(height: inner)
                    .background {
                        if chosen {
                            Capsule()
                                .fill(LinearGradient(colors: [LP.Surface.raisedTop.color, LP.Surface.raisedBottom.color], startPoint: .top, endPoint: .bottom))
                                .overlay(InsetShadow(Capsule(), color: LPColor.white.opacity(0.7), y: 1))
                                .shadow(color: Color.black.opacity(0.25), radius: 1, x: 0, y: 1)
                                .matchedGeometryEffect(id: "thumb", in: thumb)
                        }
                    }
                    .contentShape(Rectangle())
                    .onTapGesture {
                        withAnimation(reduceMotion ? nil : LP.Motion.easeSpring.animation(duration: LP.Motion.normal)) {
                            selection = option.value
                        }
                    }
            }
        }
        .padding(LP.Size.segmentedPad)
        .frame(height: height)
        .background {
            ZStack {
                track.fill(LP.Platinum._3.color)
                WellEmboss(track)
            }
        }
    }
}

private struct Embossed: ViewModifier {
    var active: Bool

    func body(content: Content) -> some View {
        if active { content.lpEmbossed() } else { content }
    }
}

/// Lays its children side by side, each as wide as the widest.
struct EqualWidths: Layout {
    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let sizes = subviews.map { $0.sizeThatFits(.unspecified) }
        let width = sizes.map(\.width).max() ?? 0
        return CGSize(width: width * CGFloat(subviews.count), height: sizes.map(\.height).max() ?? 0)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let width = subviews.map { $0.sizeThatFits(.unspecified).width }.max() ?? 0
        for (index, subview) in subviews.enumerated() {
            subview.place(at: CGPoint(x: bounds.minX + CGFloat(index) * width, y: bounds.minY),
                          proposal: ProposedViewSize(width: width, height: bounds.height))
        }
    }
}

/// A sheet as maryui's lp_sheet hangs one: from under the title bar, over a dimmed body, with an icon, a title,
/// a message, optional content, and its buttons on the right (the first is the default, the second cancels).
public struct LPSheet<Content: View>: View {
    public struct Action {
        public var title: String
        public var role: Role
        public var action: () -> Void

        public enum Role { case primary, cancel, other }

        public init(_ title: String, role: Role = .other, action: @escaping () -> Void) {
            self.title = title
            self.role = role
            self.action = action
        }
    }

    var symbol: String?
    var title: String
    var message: String?
    var actions: [Action]
    var content: Content

    public init(symbol: String? = nil, title: String, message: String? = nil, actions: [Action], @ViewBuilder content: () -> Content) {
        self.symbol = symbol
        self.title = title
        self.message = message
        self.actions = actions
        self.content = content()
    }

    public var body: some View {
        GeometryReader { geometry in
            let bodyWidth = geometry.size.width
            let width = bodyWidth - 48 >= 240 ? min(440, bodyWidth - 48) : min(240, bodyWidth - 16)
            ZStack(alignment: .top) {
                Color.black.opacity(0.08).contentShape(Rectangle())
                panel.frame(width: width).offset(y: -14)
            }
            .frame(width: geometry.size.width, height: geometry.size.height)
            .clipped()
        }
    }

    private var panel: some View {
        let shape = RoundedRectangle(cornerRadius: LP.Radius.lg, style: .continuous)
        return VStack(alignment: .leading, spacing: LP.Space._5) {
            HStack(alignment: .top, spacing: LP.Space._4) {
                if let symbol {
                    Image(systemName: symbol)
                        .font(.system(size: 30, weight: .light))
                        .foregroundStyle(LP.Ink.secondary.color)
                        .frame(width: 40, height: 40)
                }
                VStack(alignment: .leading, spacing: LP.Space._2) {
                    Text(title)
                        .font(.system(size: LP.Text.md, weight: .bold))
                        .foregroundStyle(LP.Ink.primary.color)
                        .lpEmbossed()
                    if let message {
                        Text(message)
                            .font(.system(size: LP.Text.sm))
                            .foregroundStyle(LP.Ink.secondary.color)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    content
                }
            }
            HStack(spacing: LP.Space._2) {
                ForEach(actions.indices.filter { actions[$0].role == .other }, id: \.self) { button(actions[$0]) }
                Spacer(minLength: 0)
                ForEach(actions.indices.filter { actions[$0].role == .cancel }, id: \.self) { button(actions[$0]) }
                ForEach(actions.indices.filter { actions[$0].role == .primary }, id: \.self) { button(actions[$0]) }
            }
        }
        .padding(LP.Space._5)
        .padding(.top, 14)
        .background {
            ZStack {
                ForEach((1...4).reversed(), id: \.self) { k in
                    RoundedRectangle(cornerRadius: LP.Radius.lg + CGFloat(k), style: .continuous)
                        .fill(Color.black.opacity(0.035))
                        .padding(.horizontal, -CGFloat(k))
                        .padding(.bottom, -CGFloat(k))
                }
                shape.stroke(LP.Edge.hairline.color, lineWidth: 2)
                BrushedSurface(.flat, in: shape)
            }
        }
    }

    private func button(_ action: Action) -> some View {
        Button(action: action.action) {
            Text(action.title).frame(minWidth: 76 - 2 * LP.Space._3)
        }
        .buttonStyle(LPPillButtonStyle(action.role == .primary ? .primary : .normal))
        .keyboardShortcut(action.role == .primary ? .defaultAction : (action.role == .cancel ? .cancelAction : nil))
    }
}

extension LPSheet where Content == EmptyView {
    public init(symbol: String? = nil, title: String, message: String? = nil, actions: [Action]) {
        self.init(symbol: symbol, title: title, message: message, actions: actions) { EmptyView() }
    }
}
