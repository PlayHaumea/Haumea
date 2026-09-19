import Foundation

class EmulatorCore: ObservableObject {
    static let shared = EmulatorCore()
    
    @Published var isRunning = false
    @Published var currentFPS: Double = 0.0
    
    private var timer: Timer?
    private var lastFrames: UInt64 = 0
    
    private init() {}
    
    func bootGame(path: String) -> Bool {
        let result = path.withCString { cStr in
            haumea_boot_game(cStr)
        }
        if result {
            isRunning = true
            startStatsTimer()
        }
        return result
    }
    
    func setPaused(_ paused: Bool) {
        haumea_set_paused(paused)
    }
    
    func stopGame() {
        // Assume there is a way to stop, or we just rely on pause/kill.
        // haumea_stop() doesn't exist in embed.h, but state can be checked.
        timer?.invalidate()
        isRunning = false
    }
    
    private func startStatsTimer() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            guard let self = self else { return }
            let current = haumea_guest_frames()
            self.currentFPS = Double(current - self.lastFrames)
            self.lastFrames = current
        }
    }
    
    func sendPadButton(button: Int, down: Bool) {
        haumea_pad_button(0, Int32(button), down)
    }
}
