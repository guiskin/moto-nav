import SwiftUI
import CoreLocation

struct MaintenanceItem: Codable, Identifiable, Equatable {
    var id = UUID()
    var name: String
    var everyKm: Double?
    var everyDays: Int?
    var lastKm: Double
    var lastDate: Date

    func remainingKm(_ odo: Double) -> Double? {
        guard let e = everyKm else { return nil }
        return lastKm + e - odo
    }

    func remainingDays(_ now: Date = Date()) -> Int? {
        guard let e = everyDays else { return nil }
        let due = Calendar.current.date(byAdding: .day, value: e, to: lastDate) ?? lastDate
        return Calendar.current.dateComponents([.day], from: now, to: due).day
    }

    func isDue(_ odo: Double) -> Bool {
        if let k = remainingKm(odo), k <= 300 { return true }
        if let d = remainingDays(), d <= 15 { return true }
        return false
    }

    func summary(_ odo: Double) -> String {
        var parts: [String] = []
        if let k = remainingKm(odo) {
            parts.append(k <= 0 ? "atrasada \(Int(-k)) km" : "em \(Int(k)) km")
        }
        if let d = remainingDays() {
            parts.append(d <= 0 ? "atrasada \(-d) dias" : "em \(d) dias")
        }
        return parts.joined(separator: " ou ")
    }
}

@MainActor
final class MaintenanceStore: ObservableObject {
    @Published var items: [MaintenanceItem] = [] { didSet { save() } }
    @Published var odometer: Double = 0 { didSet { UserDefaults.standard.set(odometer, forKey: "odometerKm") } }
    private var lastLoc: CLLocation?
    private var lastAlert: [String: Date] = [:]

    init() {
        let d = UserDefaults.standard
        odometer = d.double(forKey: "odometerKm")
        if let data = d.data(forKey: "maintenanceItems"), let v = try? JSONDecoder().decode([MaintenanceItem].self, from: data) {
            items = v
        } else {
            items = [
                MaintenanceItem(name: "Troca de óleo", everyKm: 5000, everyDays: 180, lastKm: 0, lastDate: Date()),
                MaintenanceItem(name: "Revisão geral", everyKm: 10000, everyDays: 365, lastKm: 0, lastDate: Date())
            ]
        }
        if let data = d.data(forKey: "maintenanceAlerts"), let v = try? JSONDecoder().decode([String: Date].self, from: data) {
            lastAlert = v
        }
    }

    private func save() {
        if let data = try? JSONEncoder().encode(items) {
            UserDefaults.standard.set(data, forKey: "maintenanceItems")
        }
    }

    func track(_ loc: CLLocation) {
        guard loc.horizontalAccuracy >= 0, loc.horizontalAccuracy < 35 else { return }
        if let p = lastLoc {
            let d = loc.distance(from: p)
            if d >= 8 && d < 800 {
                odometer += d / 1000
                lastLoc = loc
            } else if d >= 800 {
                lastLoc = loc
            }
        } else {
            lastLoc = loc
        }
    }

    func markDone(_ item: MaintenanceItem) {
        guard let i = items.firstIndex(where: { $0.id == item.id }) else { return }
        items[i].lastKm = odometer
        items[i].lastDate = Date()
        lastAlert[item.id.uuidString] = nil
        saveAlerts()
    }

    func dueMessages() -> [String] {
        let cal = Calendar.current
        var out: [String] = []
        for it in items where it.isDue(odometer) {
            if let last = lastAlert[it.id.uuidString], cal.isDateInToday(last) { continue }
            lastAlert[it.id.uuidString] = Date()
            out.append("\(it.name) \(it.summary(odometer))")
        }
        if !out.isEmpty { saveAlerts() }
        return out
    }

    private func saveAlerts() {
        if let data = try? JSONEncoder().encode(lastAlert) {
            UserDefaults.standard.set(data, forKey: "maintenanceAlerts")
        }
    }
}

struct MaintenanceView: View {
    @EnvironmentObject var store: MaintenanceStore
    @State private var editing: MaintenanceItem?
    @State private var odoText = ""

    var body: some View {
        List {
            Section("Hodômetro") {
                HStack {
                    Text("Quilometragem")
                    Spacer()
                    TextField("km", text: $odoText)
                        .keyboardType(.decimalPad)
                        .multilineTextAlignment(.trailing)
                        .frame(width: 110)
                        .onSubmit { if let v = Double(odoText.replacingOccurrences(of: ",", with: ".")) { store.odometer = v } }
                    Text("km")
                }
            }
            Section("Itens") {
                ForEach(store.items) { it in
                    Button { editing = it } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(it.name).foregroundStyle(.primary)
                            Text(it.summary(store.odometer))
                                .font(.subheadline)
                                .foregroundStyle(it.isDue(store.odometer) ? .orange : .secondary)
                        }
                    }
                    .swipeActions(edge: .leading) {
                        Button("Feito") { store.markDone(it) }.tint(.green)
                    }
                }
                .onDelete { store.items.remove(atOffsets: $0) }
                Button {
                    editing = MaintenanceItem(name: "", everyKm: 5000, everyDays: nil, lastKm: store.odometer, lastDate: Date())
                } label: { Label("Novo item", systemImage: "plus") }
            }
            Section {
                EmptyView()
            } footer: {
                Text("O hodômetro conta os km rodados com o app em navegação ou passeio. Arraste um item para a direita e toque em Feito depois da manutenção.")
            }
        }
        .navigationTitle("Manutenção")
        .onAppear { odoText = String(Int(store.odometer.rounded())) }
        .onDisappear { if let v = Double(odoText.replacingOccurrences(of: ",", with: ".")) { store.odometer = v } }
        .sheet(item: $editing) { it in
            MaintenanceEditor(item: it) { saved in
                if let i = store.items.firstIndex(where: { $0.id == saved.id }) { store.items[i] = saved }
                else { store.items.append(saved) }
            }
        }
    }
}

struct MaintenanceEditor: View {
    @State var item: MaintenanceItem
    let onSave: (MaintenanceItem) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var km = ""
    @State private var days = ""
    @State private var lastKm = ""

    var body: some View {
        NavigationStack {
            Form {
                TextField("Nome (ex.: Pneu traseiro)", text: $item.name)
                TextField("A cada quantos km (vazio = não usar)", text: $km).keyboardType(.numberPad)
                TextField("A cada quantos dias (vazio = não usar)", text: $days).keyboardType(.numberPad)
                TextField("Última feita no km", text: $lastKm).keyboardType(.decimalPad)
                DatePicker("Data da última", selection: $item.lastDate, displayedComponents: .date)
            }
            .navigationTitle("Item")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancelar") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Salvar") {
                        item.everyKm = Double(km)
                        item.everyDays = Int(days)
                        item.lastKm = Double(lastKm.replacingOccurrences(of: ",", with: ".")) ?? item.lastKm
                        if !item.name.trimmingCharacters(in: .whitespaces).isEmpty, item.everyKm != nil || item.everyDays != nil {
                            onSave(item)
                        }
                        dismiss()
                    }
                }
            }
            .onAppear {
                km = item.everyKm.map { String(Int($0)) } ?? ""
                days = item.everyDays.map { String($0) } ?? ""
                lastKm = String(Int(item.lastKm.rounded()))
            }
        }
    }
}
