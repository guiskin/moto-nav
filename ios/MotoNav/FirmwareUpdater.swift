import Foundation

@MainActor
final class FirmwareUpdater: ObservableObject {
    @Published private(set) var latest: Int?
    @Published private(set) var checking = false
    @Published private(set) var downloading = false
    @Published private(set) var error: String?

    private let base = "https://github.com/guiskin/moto-nav/releases/download/firmware/"

    func check() async {
        checking = true
        error = nil
        defer { checking = false }
        guard let url = URL(string: base + "version.txt") else { return }
        var req = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 20)
        req.setValue("MotoNav", forHTTPHeaderField: "User-Agent")
        do {
            let (data, _) = try await URLSession.shared.data(for: req)
            latest = Int(String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines))
        } catch {
            self.error = "Não foi possível verificar a versão"
        }
    }

    func install(on ble: BLEManager) async {
        guard let url = URL(string: base + "MotoNav-app.bin") else { return }
        downloading = true
        error = nil
        defer { downloading = false }
        var req = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 60)
        req.setValue("MotoNav", forHTTPHeaderField: "User-Agent")
        do {
            let (data, resp) = try await URLSession.shared.data(for: req)
            guard (resp as? HTTPURLResponse)?.statusCode == 200, data.count > 100_000, data.first == 0xE9 else {
                error = "Arquivo de firmware inválido"
                return
            }
            ble.sendFirmware(data)
        } catch {
            self.error = "Falha ao baixar o firmware"
        }
    }
}
