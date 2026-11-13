export function Name() { return "AULA F75 (SMK)"; }
export function VendorId() { return 0x258A; }
export function ProductId() { return 0x010C; }
export function Publisher() { return "SMK"; }
export function Documentation() { return "troubleshooting/brand"; }
export function Size() { return [15, 6]; }
export function DefaultPosition() { return [10, 100]; }
export function DefaultScale() { return 8.0; }
export function ControllableParameters() {
	return [
		{ "property": "shutdownColor", "group": "lighting", "label": "Shutdown Color", "min": "0", "max": "360", "type": "color", "default": "#000000" },
		{ "property": "LightingMode", "group": "lighting", "label": "Lighting Mode", "type": "combobox", "values": ["Canvas", "Forced"], "default": "Canvas" },
		{ "property": "forcedColor", "group": "lighting", "label": "Forced Color", "min": "0", "max": "360", "type": "color", "default": "#009bde" },
	];
}

export function Validate(endpoint) {
	return endpoint.usage === 0x0061 && endpoint.usage_page === 0xFF60;
}

export function Initialize() {
	device.set_endpoint(0xFF60, 0x0061, 1);
}

/*
 * 80 backlit keys of the AULA F75 (6 rows x 15 cols).
 * LED index in firmware = col * 6 + row.
 * Excludes the 6 keyless matrix holes and [col 14, row 0] (Caps/Status LED).
 */
const vKeys = [
	// Row 0 (F-row)
	0, 12, 18, 24, 30, 36, 42, 48, 54, 60, 66, 72, 78,
	// Row 1 (Number row)
	1, 7, 13, 19, 25, 31, 37, 43, 49, 55, 61, 67, 73, 79, 85,
	// Row 2 (QWERTY)
	2, 8, 14, 20, 26, 32, 38, 44, 50, 56, 62, 68, 74, 80, 86,
	// Row 3 (Home row)
	3, 9, 15, 21, 27, 33, 39, 45, 51, 57, 63, 69, 81, 87,
	// Row 4 (Shift row)
	4, 10, 16, 22, 28, 34, 40, 46, 52, 58, 64, 70, 82, 88,
	// Row 5 (Bottom row)
	5, 11, 17, 35, 53, 59, 77, 83, 89
];

const vLedNames = [
	"Esc", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
	"`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "Backspace", "Del",
	"Tab", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "\\", "PgUp",
	"Caps Lock", "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", "Enter", "PgDn",
	"Left Shift", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "/", "Right Shift", "Up", "End",
	"Left Ctrl", "Left Win", "Left Alt", "Space", "Fn", "Right Ctrl", "Left", "Down", "Right"
];

const vLedPositions = [
	[0, 0], [2, 0], [3, 0], [4, 0], [5, 0], [6, 0], [7, 0], [8, 0], [9, 0], [10, 0], [11, 0], [12, 0], [13, 0],
	[0, 1], [1, 1], [2, 1], [3, 1], [4, 1], [5, 1], [6, 1], [7, 1], [8, 1], [9, 1], [10, 1], [11, 1], [12, 1], [13, 1], [14, 1],
	[0, 2], [1, 2], [2, 2], [3, 2], [4, 2], [5, 2], [6, 2], [7, 2], [8, 2], [9, 2], [10, 2], [11, 2], [12, 2], [13, 2], [14, 2],
	[0, 3], [1, 3], [2, 3], [3, 3], [4, 3], [5, 3], [6, 3], [7, 3], [8, 3], [9, 3], [10, 3], [11, 3], [13, 3], [14, 3],
	[0, 4], [1, 4], [2, 4], [3, 4], [4, 4], [5, 4], [6, 4], [7, 4], [8, 4], [9, 4], [10, 4], [11, 4], [13, 4], [14, 4],
	[0, 5], [1, 5], [2, 5], [5, 5], [8, 5], [9, 5], [12, 5], [13, 5], [14, 5]
];

export function LedNames() {
	return vLedNames;
}

export function LedPositions() {
	return vLedPositions;
}

function sendFrame(rgbFlat90) {
	// 90 LEDs total, 9 LEDs per 32-byte HID packet (10 packets)
	for (let start = 0; start < 90; start += 9) {
		const packet = new Array(33).fill(0);
		packet[0] = 0x00; // Windows HID report ID
		packet[1] = 0xF0; // SRGB_CMD_SET_LEDS
		packet[2] = start;
		packet[3] = 9;
		for (let i = 0; i < 9; i++) {
			const base = (start + i) * 3;
			packet[4 + i * 3 + 0] = rgbFlat90[base + 0];
			packet[4 + i * 3 + 1] = rgbFlat90[base + 1];
			packet[4 + i * 3 + 2] = rgbFlat90[base + 2];
		}
		device.write(packet, 33);
	}
}

function grabColors(overrideColor) {
	const rgbData = new Array(90 * 3).fill(0);

	for (let i = 0; i < vKeys.length; i++) {
		const idx = vKeys[i];
		const pos = vLedPositions[i];
		let col;
		if (overrideColor) {
			col = hexToRgb(overrideColor);
		} else if (LightingMode === "Forced") {
			col = hexToRgb(forcedColor);
		} else {
			col = device.color(pos[0], pos[1]);
		}
		rgbData[idx * 3 + 0] = col[0];
		rgbData[idx * 3 + 1] = col[1];
		rgbData[idx * 3 + 2] = col[2];
	}

	return rgbData;
}

export function Render() {
	sendFrame(grabColors(false));
}

export function Shutdown(SystemSuspending) {
	const packet = new Array(33).fill(0);
	packet[0] = 0x00;
	packet[1] = 0xF1; // SRGB_CMD_EXIT: restore onboard effect
	device.write(packet, 33);
}

function hexToRgb(hex) {
	const result = /^#?([a-f\d]{2})([a-f\d]{2})([a-f\d]{2})$/i.exec(hex);
	return result ? [
		parseInt(result[1], 16),
		parseInt(result[2], 16),
		parseInt(result[3], 16)
	] : [0, 0, 0];
}

export function Image() {
	return "";
}
