import SwiftUI

struct GamepadView: View {
    var body: some View {
        VStack {
            Spacer()
            HStack {
                // D-Pad
                VStack {
                    GamepadButton(icon: "arrow.up", buttonId: Int(HAUMEA_PAD_UP.rawValue))
                    HStack {
                        GamepadButton(icon: "arrow.left", buttonId: Int(HAUMEA_PAD_LEFT.rawValue))
                        Spacer().frame(width: 40)
                        GamepadButton(icon: "arrow.right", buttonId: Int(HAUMEA_PAD_RIGHT.rawValue))
                    }
                    GamepadButton(icon: "arrow.down", buttonId: Int(HAUMEA_PAD_DOWN.rawValue))
                }
                .padding()
                
                Spacer()
                
                // Action Buttons
                VStack {
                    GamepadButton(icon: "triangle", buttonId: Int(HAUMEA_PAD_TRIANGLE.rawValue))
                    HStack {
                        GamepadButton(icon: "square", buttonId: Int(HAUMEA_PAD_SQUARE.rawValue))
                        Spacer().frame(width: 40)
                        GamepadButton(icon: "circle", buttonId: Int(HAUMEA_PAD_CIRCLE.rawValue))
                    }
                    GamepadButton(icon: "multiply", buttonId: Int(HAUMEA_PAD_CROSS.rawValue))
                }
                .padding()
            }
            .padding(.bottom, 40)
            .padding(.horizontal, 40)
        }
    }
}

struct GamepadButton: View {
    let icon: String
    let buttonId: Int
    @State private var isPressed = false
    
    var body: some View {
        Image(systemName: icon)
            .font(.largeTitle)
            .frame(width: 60, height: 60)
            .background(Circle().fill(Color.black.opacity(0.5)))
            .foregroundColor(.white)
            .scaleEffect(isPressed ? 0.9 : 1.0)
            .simultaneousGesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        if !isPressed {
                            isPressed = true
                            EmulatorCore.shared.sendPadButton(button: buttonId, down: true)
                        }
                    }
                    .onEnded { _ in
                        isPressed = false
                        EmulatorCore.shared.sendPadButton(button: buttonId, down: false)
                    }
            )
    }
}
