import SwiftUI
import MapKit

struct FavoritesView: View {
    @EnvironmentObject var places: PlacesStore
    @Environment(\.dismiss) private var dismiss
    let onSelect: (MKMapItem) -> Void
    @State private var showAdd = false
    @State private var renaming: SavedPlace?
    @State private var newName = ""

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
                                Button("Renomear") { newName = p.name; renaming = p }.tint(.blue)
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
                AddFavoriteView().environmentObject(places)
            }
            .alert("Renomear favorito", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
                TextField("Nome", text: $newName)
                Button("Salvar") { if let r = renaming { places.renameFavorite(r, to: newName) }; renaming = nil }
                Button("Cancelar", role: .cancel) { renaming = nil }
            }
        }
    }
}

struct AddFavoriteView: View {
    @EnvironmentObject var places: PlacesStore
    @EnvironmentObject var location: LocationProvider
    @Environment(\.dismiss) private var dismiss
    @StateObject private var search = SearchModel()
    @State private var name = ""
    @State private var picked: MKMapItem?
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
                Section("Endereço") {
                    TextField("Rua, número e cidade", text: $search.query)
                        .focused($addressFocused)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .onChange(of: search.query) { _, v in
                            if v != pickedText {
                                picked = nil
                                failed = false
                            }
                        }
                    if picked == nil {
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
                if let item = picked {
                    Section("Confirme o endereço") {
                        let pm = item.placemark
                        VStack(alignment: .leading, spacing: 4) {
                            Text([pm.thoroughfare, pm.subThoroughfare].compactMap { $0 }.joined(separator: ", ")
                                 .ifEmpty(item.name ?? "Endereço")).font(.headline)
                            Text([pm.subLocality, pm.locality, pm.administrativeArea].compactMap { $0 }.joined(separator: " - "))
                            if let cep = pm.postalCode { Text("CEP \(cep)").foregroundStyle(.secondary) }
                            if pm.subThoroughfare == nil {
                                Text("Este resultado não tem número. Digite o número na busca, se quiser o ponto exato.")
                                    .font(.caption).foregroundStyle(.orange)
                            }
                        }
                        Map(initialPosition: .region(MKCoordinateRegion(center: pm.coordinate, latitudinalMeters: 500, longitudinalMeters: 500))) {
                            Marker(name.isEmpty ? "Destino" : name, coordinate: pm.coordinate)
                        }
                        .frame(height: 200)
                        .clipShape(RoundedRectangle(cornerRadius: 10))
                        .allowsHitTesting(false)
                        Button("Confirmar e salvar") {
                            places.addFavorite(item, name: name.isEmpty ? (item.name ?? "Favorito") : name)
                            dismiss()
                        }
                        .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
                    }
                }
            }
            .navigationTitle("Novo favorito")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancelar") { dismiss() }
                }
            }
            .onAppear {
                if let c = location.location?.coordinate { search.setRegion(around: c, meters: 2_000_000) }
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
            if let item = item {
                let text = [c.title, c.subtitle].filter { !$0.isEmpty }.joined(separator: ", ")
                pickedText = text
                picked = item
                search.setQueryQuietly(text)
            } else {
                failed = true
            }
        }
    }
}

private extension String {
    func ifEmpty(_ other: String) -> String { isEmpty ? other : self }
}
