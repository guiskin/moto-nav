import SwiftUI
import MapKit

struct TripsView: View {
    @EnvironmentObject var trips: TripStore
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Group {
                if trips.trips.isEmpty {
                    ContentUnavailableView("Nenhuma viagem", systemImage: "road.lanes",
                                           description: Text("As viagens aparecem aqui ao encerrar uma navegação ou um passeio."))
                } else {
                    List {
                        ForEach(trips.trips) { t in
                            NavigationLink {
                                TripDetailView(trip: t)
                            } label: {
                                VStack(alignment: .leading, spacing: 4) {
                                    Text(t.title).font(.headline)
                                    Text(t.start.formatted(date: .abbreviated, time: .shortened))
                                        .font(.caption).foregroundStyle(.secondary)
                                    Text("\(Format.distance(t.distance)) · \(Format.duration(t.movingTime)) · média \(Int((t.avgSpeed * 3.6).rounded())) km/h")
                                        .font(.subheadline)
                                }
                            }
                        }
                        .onDelete { trips.delete(at: $0) }
                    }
                }
            }
            .navigationTitle("Viagens")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("OK") { dismiss() }
                }
            }
        }
    }
}

struct TripDetailView: View {
    let trip: Trip

    var body: some View {
        let coords = trip.coordinates
        VStack(spacing: 0) {
            Map {
                if coords.count >= 2 {
                    MapPolyline(coordinates: coords).stroke(.blue, lineWidth: 5)
                }
                if let first = coords.first {
                    Marker("Início", systemImage: "flag", coordinate: first).tint(.green)
                }
                if let last = coords.last {
                    Marker("Fim", systemImage: "flag.checkered", coordinate: last).tint(.red)
                }
            }
            .frame(height: 320)
            List {
                row("Distância", Format.distance(trip.distance))
                row("Tempo em movimento", Format.duration(trip.movingTime))
                row("Duração total", Format.duration(trip.end.timeIntervalSince(trip.start)))
                row("Velocidade média", "\(Int((trip.avgSpeed * 3.6).rounded())) km/h")
                row("Velocidade máxima", "\(Int((trip.maxSpeed * 3.6).rounded())) km/h")
                row("Início", trip.start.formatted(date: .abbreviated, time: .shortened))
            }
        }
        .navigationTitle(trip.title)
        .navigationBarTitleDisplayMode(.inline)
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label)
            Spacer()
            Text(value).foregroundStyle(.secondary)
        }
    }
}
