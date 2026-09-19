import SwiftUI

struct GameMetadata: Identifiable, Codable {
    let id: String
    let title: String
    let developer: String?
    let version: String?
}

struct LibraryView: View {
    @State private var games: [GameMetadata] = [
        // Placeholder data
        GameMetadata(id: "PPSA00001", title: "Astro's Playroom (Stub)", developer: "Team Asobi", version: "1.00")
    ]
    
    @State private var showSettings = false
    @State private var selectedGame: GameMetadata?
    
    let columns = [GridItem(.adaptive(minimum: 150))]
    
    var body: some View {
        NavigationView {
            ZStack {
                Color.black.edgesIgnoringSafeArea(.all)
                
                ScrollView {
                    LazyVGrid(columns: columns, spacing: 20) {
                        ForEach(games) { game in
                            GameCard(game: game)
                                .onTapGesture {
                                    selectedGame = game
                                }
                        }
                    }
                    .padding()
                }
            }
            .navigationTitle("Haumea")
            .navigationBarItems(trailing: Button(action: {
                showSettings.toggle()
            }) {
                Image(systemName: "gearshape.fill")
                    .foregroundColor(.white)
            })
            .sheet(isPresented: $showSettings) {
                SettingsView()
            }
            .fullScreenCover(item: $selectedGame) { game in
                GameRunnerView(game: game)
            }
        }
    }
}

struct GameCard: View {
    let game: GameMetadata
    
    var body: some View {
        VStack {
            Rectangle()
                .fill(Color.gray.opacity(0.3))
                .aspectRatio(1.0, contentMode: .fit)
                .cornerRadius(12)
                .overlay(
                    Text("No Art")
                        .foregroundColor(.gray)
                )
            
            Text(game.title)
                .font(.headline)
                .foregroundColor(.white)
                .lineLimit(1)
            
            if let dev = game.developer {
                Text(dev)
                    .font(.subheadline)
                    .foregroundColor(.gray)
                    .lineLimit(1)
            }
        }
    }
}

struct GameRunnerView: View {
    let game: GameMetadata
    @Environment(\.presentationMode) var presentationMode
    
    var body: some View {
        ZStack {
            EmulatorView()
                .edgesIgnoringSafeArea(.all)
                
            GamepadView()
            
            VStack {
                HStack {
                    Button(action: {
                        EmulatorCore.shared.stopGame()
                        presentationMode.wrappedValue.dismiss()
                    }) {
                        Image(systemName: "xmark.circle.fill")
                            .font(.largeTitle)
                            .foregroundColor(.white.opacity(0.7))
                    }
                    .padding()
                    Spacer()
                }
                Spacer()
            }
        }
        .onAppear {
            _ = EmulatorCore.shared.bootGame(path: game.id)
        }
    }
}
