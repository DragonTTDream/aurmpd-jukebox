/*
 * Shared WebSocket connection to the aurmpd backend.
 *
 * Commands are queued while the socket is still CONNECTING and flushed once it
 * opens, so code that runs early (a component constructor, for example) can no
 * longer throw "InvalidStateError: Sent before connected" — that exception used
 * to abort the first render and leave a completely blank page.
 *
 * Commands issued while the socket is CLOSED/CLOSING are dropped with a console
 * error instead of throwing, so a backend restart degrades visibly rather than
 * taking the UI down.
 */
const currentHostname = window.location.host;

export const socket = new WebSocket('ws://' + currentHostname + '/ws');

const pending = [];

socket.addEventListener('open', function () {
	while (pending.length) {
		var cmd = pending.shift();
		try {
			socket.send(cmd);
		} catch (e) {
			console.error('WebSocket send failed:', cmd, e);
		}
	}
});

export function sendCommand(cmd) {
	if (socket.readyState === WebSocket.OPEN) {
		socket.send(cmd);
	} else if (socket.readyState === WebSocket.CONNECTING) {
		pending.push(cmd);
	} else {
		console.error('WebSocket is not connected, command dropped:', cmd);
	}
}
