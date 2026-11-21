# Hardware

## Plataforma de desenvolvimento

**ESP32 DevKitC (módulo ESP32-WROOM-32)** — microcontrolador Xtensa LX6 dual-core de 32 bits, até 240 MHz,
520 KB de SRAM, 4 MB de flash, Wi-Fi 802.11 b/g/n e Bluetooth integrados, lógica de 3,3 V. O Wi-Fi
integrado dispensa módulo de comunicação externo e ponte serial: o próprio ESP32 abre a conexão TCP com o
broker MQTT.

## Componentes

| Componente | Função | Especificação principal | Simulação (Wokwi) |
|---|---|---|---|
| ESP32 DevKitC WROOM-32 | controlador + Wi-Fi | 3,3 V, USB-C | `board-esp32-devkit-c-v4` |
| MAX30102 | **sensor** de pulso (PPG) | I2C 0x57, LEDs vermelho/IR, fotodetector | potenciômetro (BPM) + chave (dedo) |
| OLED 0,96" SSD1306 | display local | 128×64, I2C 0x3C | `board-ssd1306` |
| Buzzer ativo 3 V, 12 mm | **atuador** sonoro | bip contínuo | `wokwi-buzzer` (piezo, acionado com `tone`) |
| Transistor BC547 + resistor 200 Ω | chaveamento do buzzer | NPN, base via 200 Ω | não usado (buzzer direto no GPIO) |
| LED vermelho 5 mm + resistor 220 Ω | **atuador** visual, espelha o buzzer | ~10 mA | `wokwi-led` + `wokwi-resistor` |
| Protoboard 830 pontos | montagem sem solda | — | — |
| Cabos Dupont (M-M, M-F, F-F) | ligações | 20 cm, 24 AWG | fios do diagrama |

O Wokwi não oferece o MAX30102. Na simulação, o potenciômetro (GPIO 34) fornece a frequência de
referência, de 30 a 180 BPM, e a chave deslizante (GPIO 27) indica a presença do dedo. O firmware gera
os batimentos nesse ritmo, com ±2 % de variação, e aplica o mesmo cálculo de BPM, alarme e publicação
usado com o sensor real.

## Pinagem

| ESP32 | Ligação |
|---|---|
| 3V3 | VIN do MAX30102, VCC do OLED, positivo do buzzer (físico), VCC do potenciômetro |
| GND | GND comum de todos os módulos, emissor do BC547, cátodo do LED (via 220 Ω) |
| GPIO 21 (SDA) | SDA do MAX30102 e do OLED |
| GPIO 22 (SCL) | SCL do MAX30102 e do OLED |
| GPIO 25 | físico: resistor 200 Ω → base do BC547 (coletor no negativo do buzzer). Wokwi: pino + do buzzer |
| GPIO 26 | ânodo do LED → LED → 220 Ω → GND |
| GPIO 34 | SIG do potenciômetro (somente simulação) |
| GPIO 27 | chave deslizante para GND (somente simulação; pull-up interno) |

MAX30102 e OLED dividem o barramento I2C por terem endereços diferentes (0x57 e 0x3C).

## Alimentação

O conjunto é alimentado pela USB do ESP32 (5 V). O regulador da placa fornece 3,3 V aos módulos. Consumo
estimado: ESP32 com Wi-Fi ~80–160 mA, OLED ~20 mA, MAX30102 até ~50 mA em pulsos de LED, buzzer ~30 mA,
LED ~10 mA, abaixo da capacidade do regulador de 3,3 V da DevKitC. Antes da montagem física, deve-se
confirmar no módulo MAX30102 comprado se ele aceita 3,3 V em VIN (a maioria dos breakouts tem
reguladores de 1,8 V e 3,3 V).

## Peças mecânicas

Não há peças impressas em 3D nem caixa. O protótipo é montado em protoboard de 830 pontos
(165 × 55 mm). Uma evolução natural seria uma pulseira ou clipe de dedo que mantenha o MAX30102 sob
pressão constante e protegido da luz ambiente.
