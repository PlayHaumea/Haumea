import SwiftUI

@main
struct HaumeaApp: App {
    var body: some Scene {
        WindowGroup {
            LibraryView()
                .preferredColorScheme(.dark)
        }
    }
}
