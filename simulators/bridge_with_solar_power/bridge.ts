import { ServerNode, Logger } from "@matter/main";
import { MeasurementType } from "@matter/main/types";
import { AggregatorEndpoint } from "@matter/main/endpoints/aggregator";
import { SolarPowerDevice } from "@matter/main/devices/solar-power";
import { ElectricalPowerMeasurementServer } from "@matter/main/behaviors/electrical-power-measurement";
import { PowerTopologyServer } from "@matter/main/behaviors/power-topology";
import { BridgedDeviceBasicInformationServer } from "@matter/main/behaviors/bridged-device-basic-information";

const logger = Logger.get("SolarBridge");

// Fake data state — values drift slowly each tick
let activePowerMw = 3000_000;       // 3 kW in mW
let voltageMs = 230_000;            // 230 V in mV
let frequencyMhz = 50_000;          // 50 Hz in mHz

function jitter(value: number, maxDelta: number): number {
    return Math.round(value + (Math.random() - 0.5) * 2 * maxDelta);
}

const node = new ServerNode({
    id: "solar-bridge",
    basicInformation: {
        vendorName: "Solar Simulator",
        productName: "Solar Bridge",
        vendorId: 0xfff1,
        productId: 0x8001,
        serialNumber: "SOLAR-SIM-0001",
    },
});

const aggregator = await node.add(AggregatorEndpoint, { id: "aggregator" });

const solarEndpoint = await aggregator.add(
    SolarPowerDevice.with(
        ElectricalPowerMeasurementServer.with("AlternatingCurrent"),
        PowerTopologyServer.with("NodeTopology"),
        BridgedDeviceBasicInformationServer,
    ),
    {
        id: "solar-power-1",
        bridgedDeviceBasicInformation: {
            vendorName: "Solar Simulator",
            productName: "Virtual Solar Panels",
            nodeLabel: "Solar",
            reachable: true,
        },
        powerTopology: {},
        electricalPowerMeasurement: {
            powerMode: 2, // AC single-phase
            numberOfMeasurementTypes: 3,
            accuracy: [
                {
                    measurementType: MeasurementType.ActivePower,
                    measured: true,
                    minMeasuredValue: 0,
                    maxMeasuredValue: 10_000_000,
                    accuracyRanges: [{ rangeMin: 0, rangeMax: 10_000_000, percentMax: 100 }],
                },
                {
                    measurementType: MeasurementType.RmsVoltage,
                    measured: true,
                    minMeasuredValue: 200_000,
                    maxMeasuredValue: 260_000,
                    accuracyRanges: [{ rangeMin: 200_000, rangeMax: 260_000, percentMax: 100 }],
                },
                {
                    measurementType: MeasurementType.Frequency,
                    measured: true,
                    minMeasuredValue: 49_000,
                    maxMeasuredValue: 51_000,
                    accuracyRanges: [{ rangeMin: 49_000, rangeMax: 51_000, percentMax: 100 }],
                },
            ],
            activePower: activePowerMw,
            voltage: voltageMs,
            frequency: frequencyMhz,
        },
    },
);

await node.start();
logger.info("Solar bridge started");

// Update fake sensor values every 5 seconds
setInterval(async () => {
    // Slowly drift power between 1 kW and 6 kW
    activePowerMw = Math.max(1_000_000, Math.min(6_000_000, jitter(activePowerMw, 100_000)));
    // Voltage ±2V
    voltageMs = Math.max(220_000, Math.min(240_000, jitter(voltageMs, 500)));
    // Frequency ±0.2Hz
    frequencyMhz = Math.max(49_500, Math.min(50_500, jitter(frequencyMhz, 50)));

    await solarEndpoint.setStateOf(ElectricalPowerMeasurementServer, {
        activePower: activePowerMw,
        voltage: voltageMs,
        frequency: frequencyMhz,
    });

    logger.info(
        `Solar: power=${(activePowerMw / 1000).toFixed(0)} W  voltage=${(voltageMs / 1000).toFixed(1)} V  freq=${(frequencyMhz / 1000).toFixed(2)} Hz`,
    );
}, 5000);
