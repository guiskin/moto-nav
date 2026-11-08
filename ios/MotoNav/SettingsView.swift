import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var settings: SettingsStore
    @EnvironmentObject var ble: BLEManager
    @EnvironmentObject var engine: NavigationEngine
    @StateObject private var updater = FirmwareUpdater()
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section("Tela da placa") {
                    VStack(alignment: .leading) {
                        Text("Brilho de dia: \(Int(settings.dayBrightness * 100 / 255))%")
                        Slider(value: $settings.dayBrightness, in: 30...255, step: 5)
                    }
                    VStack(alignment: .leading) {
                        Text("Brilho à noite: \(Int(settings.nightBrightness * 100 / 255))%")
                        Slider(value: $settings.nightBrightness, in: 20...255, step: 5)
                    }
                    Picker("Modo noite", selection: $settings.nightMode) {
                        ForEach(SettingsStore.NightMode.allCases) { m in
                            Text(m.label).tag(m)
                        }
                    }
                    Toggle("Notificações do iPhone na placa", isOn: $settings.boardNotifications)
                }

                Section("Alertas") {
                    Toggle("Avisos por voz", isOn: $engine.voiceEnabled)
                    Toggle("Aviso de velocidade acima do limite", isOn: $settings.speedAlerts)
                    Toggle("Aviso de radar", isOn: $settings.radarAlerts)
                }

                Section {
                    HStack {
                        Text("Versão na placa")
                        Spacer()
                        Text(ble.boardFirmware.map { "\($0)" } ?? (ble.isConnected ? "antiga" : "—"))
                            .foregroundStyle(.secondary)
                    }
                    HStack {
                        Text("Versão disponível")
                        Spacer()
                        if updater.checking {
                            ProgressView()
                        } else {
                            Text(updater.latest.map { "\($0)" } ?? "—").foregroundStyle(.secondary)
                        }
                    }
                    if ble.otaRunning {
                        VStack(alignment: .leading, spacing: 6) {
                            ProgressView(value: ble.otaProgress)
                            Text(ble.otaMessage ?? "").font(.caption).foregroundStyle(.secondary)
                        }
                    } else {
                        Button(updater.downloading ? "Baixando..." : "Atualizar a placa") {
                            Task { await updater.install(on: ble) }
                        }
                        .disabled(!ble.isConnected || !ble.supportsOTA || updater.downloading || engine.isActive)
                        if let m = ble.otaMessage {
                            Text(m).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                    if let e = updater.error {
                        Text(e).font(.caption).foregroundStyle(.red)
                    }
                    if ble.isConnected && !ble.supportsOTA {
                        Text("Esta versão da placa ainda não atualiza pelo app. Grave uma vez pelo USB.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                } header: {
                    Text("Firmware da placa")
                } footer: {
                    Text("Mantenha o app aberto e a placa perto do iPhone durante a atualização.")
                }

                Section {
                    Button("Esquecer a placa e procurar de novo", role: .destructive) {
                        ble.forgetDevice()
                    }
                }
            }
            .navigationTitle("Ajustes")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("OK") { dismiss() }
                }
            }
            .task { await updater.check() }
            .onChange(of: settings.dayBrightness) { _, _ in engine.pushSettings() }
            .onChange(of: settings.nightBrightness) { _, _ in engine.pushSettings() }
            .onChange(of: settings.nightMode) { _, _ in engine.pushSettings() }
            .onChange(of: settings.boardNotifications) { _, _ in engine.pushSettings() }
        }
    }
}
