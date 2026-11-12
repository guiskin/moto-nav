import SwiftUI
import MapKit

struct FavoritesView: View {
    @EnvironmentObject var places: PlacesStore
    @Environment(\.dismiss) private var dismiss
    let onSelect: (MKMapItem) -> Void
    @State private var showAdd = false
    @State private var editing: SavedPlace?

    var body: some View {
        NavigationStack {
            Group {
                if places.favorites.isEmpty {
                    ContentUnavailableView("Nenhum favorito", systemImage: "star",
                                           description: Text("Toque em + para adicionar um endereço."))
                } else {
                    List {
                        ForEach(places.favorites) { p in
                            Button {
                                onSelect(p.mapItem)
                            } label: {
                                HStack(spacing: 12) {
                                    Image(systemName: "star.fill").foregroundStyle(.yellow)
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text(p.name).font(.headline).foregroundStyle(.primary)
                                        if !p.subtitle.isEmpty {
                                            Text(p.subtitle).font(.caption).foregroundStyle(.secondary)
                                        }
                                    }
                                }
                            }
                            .swipeActions {
                                Button("Excluir", role: .destructive) { places.removeFavorite(p) }
                                Button("Editar") { editing = p }.tint(.blue)
                            }
                        }
                    }
                }
            }
            .navigationTitle("Favoritos")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Fechar") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button { showAdd = true } label: { Image(systemName: "plus") }
                }
            }
            .sheet(isPresented: $showAdd) {
                AddFavoriteView(editing: nil).environmentObject(places)
            }
            .sheet(item: $editing) { p in
                AddFavoriteView(editing: p).environmentObject(places)
            }
        }
    }
}

struct AddFavoriteView: View {
    let editing: SavedPlace?
    @EnvironmentObject var places: PlacesStore
    @EnvironmentObject var location: LocationProvider
    @Environment(\.dismiss) private var dismiss
    @StateObject private var search = SearchModel()
    @State private var name = ""
    @State private var addressTitle = ""
    @State private var addressSub = ""
    @State private var coordinate: CLLocationCoordinate2D?
    @State private var noNumber = false
    @State private var camera: MapCameraPosition = .automatic
    @State private var resolving = false
    @State private var failed = false
    @State private var pickedText = ""
    @FocusState private var addressFocused: Bool

    var body: some View {
        NavigationStack {
            Form {
                Section("Nome do favorito") {
                    TextField("Ex.: Casa, Casa de praia, Academia", text: $name)
                }
                Section(coordinate == nil ? "Endereço" : "Buscar outro endereço") {
                    TextField("Rua, número e cidade", text: $search.query)
                        .focused($addressFocused)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    if search.query != pickedText {
                        ForEach(Array(search.results.prefix(6).enumerated()), id: \.offset) { _, c in
                            Button {
                                choose(c)
                            } label: {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(c.title).foregroundStyle(.primary)
                                    if !c.subtitle.isEmpty {
                                        Text(c.subtitle).font(.caption).foregroundStyle(.secondary)
                                    }
                                }
                            }
                        }
                        if resolving { ProgressView() }
                        if failed { Text("Não foi possível localizar esse endereço.").font(.caption).foregroundStyle(.red) }
                    }
                }
                if coordinate != nil {
                    Section("Confirme o endereço") {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(addressTitle).font(.headline)
                            if !addressSub.isEmpty { Text(addressSub) }
                            if noNumber {
                                Text("Este resultado não tem número. Digite o número na busca, se quiser o ponto exato.")
                                    .font(.caption).foregroundStyle(.orange)
                            }
                        }
                        ZStack {
                            Map(position: $camera, interactionModes: [.pan, .zoom]) {}
                                .onMapCameraChange(frequency: .onEnd) { ctx in
                                    coordinate = ctx.region.center
                                }
                            Image(systemName: "mappin")
                                .font(.system(size: 34))
                                .foregroundStyle(.red)
                                .offset(y: -17)
                                .allowsHitTesting(false)
                        }
                        .frame(height: 280)
                        .clipShape(RoundedRectangle(cornerRadius: 10))
                        Text("Arraste o mapa para deixar o pino no ponto certo.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    Section {
                        Button(editing == nil ? "Confirmar e salvar" : "Salvar alterações") {
                            if let c = coordinate {
                                places.saveFavorite(replacing: editing, name: name, subtitle: addressSub.isEmpty ? addressTitle : "\(addressTitle) - \(addressSub)", coordinate: c)
                                dismiss()
                            }
                        }
                        .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
                    }
                }
            }
            .navigationTitle(editing == nil ? "Novo favorito" : "Editar favorito")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancelar") { dismiss() }
                }
            }
            .onAppear {
                if let c = location.location?.coordinate { search.setRegion(around: c, meters: 2_000_000) }
                if let e = editing, coordinate == nil {
                    name = e.name
                    addressTitle = e.subtitle.isEmpty ? e.name : e.subtitle
                    addressSub = ""
                    let c = CLLocationCoordinate2D(latitude: e.latitude, longitude: e.longitude)
                    coordinate = c
                    camera = .region(MKCoordinateRegion(center: c, latitudinalMeters: 400, longitudinalMeters: 400))
                }
            }
        }
    }

    private func choose(_ c: MKLocalSearchCompletion) {
        resolving = true
        failed = false
        addressFocused = false
        Task {
            let item = await search.resolve(c)
            resolving = false
            guard let item = item else { failed = true; return }
            let pm = item.placemark
            let street = [pm.thoroughfare, pm.subThoroughfare].compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: ", ")
            addressTitle = street.isEmpty ? (item.name ?? c.title) : street
            addressSub = [pm.subLocality, pm.locality, pm.administrativeArea, pm.postalCode.map { "CEP \($0)" }]
                .compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: " - ")
            noNumber = pm.subThoroughfare == nil
            coordinate = pm.coordinate
            camera = .region(MKCoordinateRegion(center: pm.coordinate, latitudinalMeters: 400, longitudinalMeters: 400))
            let text = [c.title, c.subtitle].filter { !$0.isEmpty }.joined(separator: ", ")
            pickedText = text
            search.setQueryQuietly(text)
        }
    }
}
