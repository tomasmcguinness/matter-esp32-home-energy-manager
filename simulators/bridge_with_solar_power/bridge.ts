import { ServerNode, Logger } from "@matter/main";
import { MeasurementType } from "@matter/main/types";
import { AggregatorEndpoint } from "@matter/main/endpoints/aggregator";
import { SolarPowerDevice } from "@matter/main/devices/solar-power";
import { ElectricalSensorEndpoint } from "@matter/main/endpoints/electrical-sensor";
import { ElectricalPowerMeasurementServer } from "@matter/main/behaviors/electrical-power-measurement";
import { PowerTopologyServer } from "@matter/main/behaviors/power-topology";
import { PowerSourceServer } from "@matter/main/behaviors/power-source";
import { BridgedDeviceBasicInformationServer } from "@matter/main/behaviors/bridged-device-basic-information";

const logger = Logger.get("SolarBridge");

// Fake data state — values drift slowly each tick
let activePowerMw = 3000_000;       // 3 kW in mW  (solar output)
let voltageMv = 230_000;            // 230 V in mV  (shared AC bus)
const homeLoadMw = 2_000_000;       // 2 kW simulated home consumption (fixed)

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
            numberOfMeasurementTypes: 2,
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
            ],
            activePower: activePowerMw,
            voltage: voltageMv,
        },
    },
);

// Grid electrical sensor — measures import/export at the meter.
// activePower = homeLoad − solarOutput: positive = importing, negative = exporting.
// PowerSource tagList uses the Matter "Power Source" common namespace (0x07), tag 0x01 = Grid.
const gridEndpoint = await aggregator.add(
    ElectricalSensorEndpoint.with(
        PowerSourceServer.with("Wired"),
        PowerTopologyServer.with("NodeTopology"),
        ElectricalPowerMeasurementServer.with("AlternatingCurrent"),
        BridgedDeviceBasicInformationServer,
    ),
    {
        id: "grid-sensor",
        bridgedDeviceBasicInformation: {
            vendorName: "Solar Simulator",
            productName: "Grid Electrical Sensor",
            nodeLabel: "Grid",
            reachable: true,
        },
        powerTopology: {},
        powerSource: {
            status: 1,           // Active
            order: 1,
            description: "Grid",
            wiredCurrentType: 0, // Alternating Current
            //tagList: [{ mfgCode: null, namespaceId: 0x07, tag: 0x01 }],
        },
        electricalPowerMeasurement: {
            powerMode: 2, // AC single-phase
            numberOfMeasurementTypes: 2,
            accuracy: [
                {
                    measurementType: MeasurementType.ActivePower,
                    measured: true,
                    minMeasuredValue: -10_000_000,
                    maxMeasuredValue: 10_000_000,
                    accuracyRanges: [{ rangeMin: -10_000_000, rangeMax: 10_000_000, percentMax: 100 }],
                },
                {
                    measurementType: MeasurementType.RmsVoltage,
                    measured: true,
                    minMeasuredValue: 200_000,
                    maxMeasuredValue: 260_000,
                    accuracyRanges: [{ rangeMin: 200_000, rangeMax: 260_000, percentMax: 100 }],
                },
            ],
            activePower: homeLoadMw - activePowerMw,
            voltage: voltageMv,
        },
    },
);

await node.start();
logger.info("Solar bridge started");

// Update fake sensor values every 5 seconds
setInterval(async () => {
    // Slowly drift power between 1 kW and 6 kW
    activePowerMw = Math.max(1_000_000, Math.min(6_000_000, jitter(activePowerMw, 100_000)));
    // Voltage ±2V around 230V
    voltageMv = Math.max(220_000, Math.min(240_000, jitter(voltageMv, 500)));

    const gridImportMw = homeLoadMw - activePowerMw;

    await solarEndpoint.setStateOf(ElectricalPowerMeasurementServer, {
        activePower: activePowerMw,
        voltage: voltageMv,
    });

    await gridEndpoint.setStateOf(ElectricalPowerMeasurementServer, {
        activePower: gridImportMw,
        voltage: voltageMv,
    });

    logger.info(
        `Solar: ${(activePowerMw / 1000).toFixed(0)} W  Grid: ${(gridImportMw / 1000).toFixed(0)} W (${gridImportMw < 0 ? "exporting" : "importing"})  Voltage: ${(voltageMv / 1000).toFixed(1)} V`,
    );
}, 5000);
