import Foundation
import CoreBluetooth

final class BLEManager: NSObject, ObservableObject {
    static let serviceUUID = CBUUID(string: "6E0A0001-7A3B-4C2D-9E1F-0B5A4D3C2B1A")
    static let navCharUUID = CBUUID(string: "6E0A0002-7A3B-4C2D-9E1F-0B5A4D3C2B1A")
    private static let savedPeripheralKey = "motonav.peripheral"

    @Published private(set) var isConnected = false
    @Published private(set) var statusText = "Procurando a placa..."

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var navChar: CBCharacteristic?
    private var lastPacket: Data?

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    var maxPayload: Int {
        guard let p = peripheral else { return 20 }
        return max(20, p.maximumWriteValueLength(for: .withoutResponse))
    }

    func send(_ data: Data) {
        lastPacket = data
        guard let p = peripheral, let c = navChar, p.state == .connected else { return }
        let payload = data.prefix(maxPayload)
        if c.properties.contains(.writeWithoutResponse) && p.canSendWriteWithoutResponse {
            p.writeValue(payload, for: c, type: .withoutResponse)
        } else {
            p.writeValue(payload, for: c, type: .withResponse)
        }
    }

    private func startScan() {
        guard central.state == .poweredOn else { return }
        if let idString = UserDefaults.standard.string(forKey: Self.savedPeripheralKey),
           let id = UUID(uuidString: idString),
           let known = central.retrievePeripherals(withIdentifiers: [id]).first {
            connect(known)
            return
        }
        let connected = central.retrieveConnectedPeripherals(withServices: [Self.serviceUUID])
        if let p = connected.first {
            connect(p)
            return
        }
        statusText = "Procurando a placa..."
        central.scanForPeripherals(withServices: [Self.serviceUUID], options: nil)
    }

    private func connect(_ p: CBPeripheral) {
        peripheral = p
        p.delegate = self
        statusText = "Conectando..."
        central.connect(p, options: nil)
    }

    func forgetDevice() {
        UserDefaults.standard.removeObject(forKey: Self.savedPeripheralKey)
        if let p = peripheral { central.cancelPeripheralConnection(p) }
        peripheral = nil
        navChar = nil
        isConnected = false
        startScan()
    }
}

extension BLEManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            startScan()
        case .unauthorized:
            statusText = "Permita o Bluetooth nos Ajustes"
        case .poweredOff:
            statusText = "Bluetooth desligado"
            isConnected = false
        default:
            statusText = "Bluetooth indisponível"
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any], rssi RSSI: NSNumber) {
        central.stopScan()
        connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: Self.savedPeripheralKey)
        statusText = "Conectando..."
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        isConnected = false
        statusText = "Falha ao conectar"
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.startScan() }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        isConnected = false
        navChar = nil
        statusText = "Placa desconectada"
        central.connect(peripheral, options: nil)
    }
}

extension BLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else {
            statusText = "Placa sem o serviço MotoNav"
            return
        }
        peripheral.discoverCharacteristics([Self.navCharUUID], for: svc)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let c = service.characteristics?.first(where: { $0.uuid == Self.navCharUUID }) else { return }
        navChar = c
        isConnected = true
        statusText = "Placa conectada"
        if let last = lastPacket { send(last) }
    }
}
