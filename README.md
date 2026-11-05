# MotoNav

Navegação seta a seta para moto: app de iPhone (rotas do Apple Maps) + placa Waveshare ESP32-S3-Touch-AMOLED-1.75-G via Bluetooth.

- `firmware/MotoNav` — firmware da placa (Arduino)
- `ios` — app do iPhone (SwiftUI, gerado com XcodeGen)
- `.github/workflows/build.yml` — gera `MotoNav.ipa` e `MotoNav.ino.merged.bin` a cada push (aba Actions → artifacts)

## Firmware

Arduino IDE 2, core `esp32` 3.3.10, bibliotecas: GFX Library for Arduino da pasta `examples/arduino/libraries` do repositório da Waveshare, U8g2, TinyGPSPlus.
Placa ESP32S3 Dev Module · Flash 16MB · Partition 16M Flash (3MB APP/9.9MB FATFS) · PSRAM OPI · USB CDC On Boot Enabled.

## Protocolo BLE

Serviço `6E0A0001-7A3B-4C2D-9E1F-0B5A4D3C2B1A`, característica `6E0A0002-…` (write / write without response).

| Byte | Campo |
| --- | --- |
| 0 | versão (1) |
| 1 | ativo (0 = sem navegação, 1 = navegando) |
| 2 | manobra (1 reto, 2 leve dir., 3 dir., 4 acentuada dir., 5 leve esq., 6 esq., 7 acentuada esq., 8 retorno, 9 rotatória, 10 chegada, 11 partida, 12 mantenha dir., 13 mantenha esq.) |
| 3 | saída da rotatória |
| 4–5 | ângulo (int16, graus, + = direita) |
| 6–9 | distância até a manobra (m) |
| 10–13 | distância restante (m) |
| 14–15 | tempo restante (min) |
| 16 | velocidade (km/h) |
| 17 | flags (bit0 = recalculando) |
| 18… | nome da rua (UTF-8) |
