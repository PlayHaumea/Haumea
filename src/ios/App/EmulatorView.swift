import SwiftUI
import UIKit

class EmulatorUIView: UIView {
    override class var layerClass: AnyClass {
        return CAMetalLayer.self
    }
    
    override init(frame: CGRect) {
        super.init(frame: frame)
        setupLayer()
    }
    
    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setupLayer()
    }
    
    private func setupLayer() {
        guard let metalLayer = self.layer as? CAMetalLayer else { return }
        metalLayer.framebufferOnly = true
        // Set the surface in Haumea
        let width = UInt32(bounds.width * UIScreen.main.scale)
        let height = UInt32(bounds.height * UIScreen.main.scale)
        let layerPtr = Unmanaged.passUnretained(metalLayer).toOpaque()
        haumea_set_surface(layerPtr, width, height)
    }
    
    override func layoutSubviews() {
        super.layoutSubviews()
        guard let metalLayer = self.layer as? CAMetalLayer else { return }
        let width = UInt32(bounds.width * UIScreen.main.scale)
        let height = UInt32(bounds.height * UIScreen.main.scale)
        let layerPtr = Unmanaged.passUnretained(metalLayer).toOpaque()
        haumea_set_surface(layerPtr, width, height)
    }
}

struct EmulatorView: UIViewRepresentable {
    func makeUIView(context: Context) -> EmulatorUIView {
        return EmulatorUIView()
    }
    
    func updateUIView(_ uiView: EmulatorUIView, context: Context) {
        // Update if needed
    }
}
