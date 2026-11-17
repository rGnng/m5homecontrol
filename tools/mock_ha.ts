// Minimal Home Assistant mock for exercising the M5 WebSocket client.
// Run: deno run --allow-net tools/mock_ha.ts

const entityId = "light.test";
let lightOn = true;
let brightness = 200;
const sockets = new Set<WebSocket>();

function stateObject() {
  return {
    entity_id: entityId,
    state: lightOn ? "on" : "off",
    attributes: {
      friendly_name: "Test Light",
      brightness: lightOn ? brightness : 0,
      supported_color_modes: ["brightness"],
    },
    last_changed: new Date().toISOString(),
    last_updated: new Date().toISOString(),
  };
}

function broadcastState() {
  const message = JSON.stringify({
    id: 1,
    type: "event",
    event: {
      event_type: "state_changed",
      data: {
        entity_id: entityId,
        old_state: null,
        new_state: stateObject(),
      },
    },
  });
  for (const socket of sockets) {
    if (socket.readyState === WebSocket.OPEN) socket.send(message);
  }
}

function json(body: unknown, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json" },
  });
}

Deno.serve({ port: 8123, hostname: "0.0.0.0" }, (req) => {
  const url = new URL(req.url);
  const upgrade = req.headers.get("upgrade")?.toLowerCase();

  if (upgrade === "websocket") {
    const { socket, response } = Deno.upgradeWebSocket(req);
    socket.onopen = () => {
      sockets.add(socket);
      console.log("[ws] open");
      socket.send(JSON.stringify({ type: "auth_required", ha_version: "mock" }));
    };
    socket.onclose = () => {
      sockets.delete(socket);
      console.log("[ws] close");
    };
    socket.onmessage = (event) => {
      const msg = JSON.parse(String(event.data));
      console.log("[ws] recv", msg);
      if (msg.type === "auth") {
        socket.send(JSON.stringify({ type: "auth_ok", ha_version: "mock" }));
      } else if (msg.type === "subscribe_events") {
        socket.send(JSON.stringify({ id: msg.id, type: "result", success: true, result: null }));
        setTimeout(broadcastState, 800);
      } else if (msg.type === "call_service") {
        socket.send(JSON.stringify({ id: msg.id, type: "result", success: true, result: null }));
      }
    };
    return response;
  }

  if (url.pathname === "/api/states") {
    console.log("[http] GET states");
    return json([
      stateObject(),
      {
        entity_id: "switch.desk_lamp",
        state: "off",
        attributes: { friendly_name: "Desk Lamp" },
        last_changed: new Date().toISOString(),
        last_updated: new Date().toISOString(),
      },
      {
        entity_id: "sensor.indoor_temperature",
        state: "23.4",
        attributes: { friendly_name: "Indoor Temperature", unit_of_measurement: "C" },
        last_changed: new Date().toISOString(),
        last_updated: new Date().toISOString(),
      },
    ]);
  }

  if (url.pathname === `/api/states/${entityId}`) {
    console.log("[http] GET state");
    return json(stateObject());
  }

  if (url.pathname.startsWith("/api/services/")) {
    console.log("[http] POST", url.pathname);
    if (url.pathname.endsWith("/light/toggle") || url.pathname.endsWith("/homeassistant/toggle")) {
      lightOn = !lightOn;
      broadcastState();
    }
    return json([]);
  }

  console.log("[http] GET", url.pathname);
  return new Response("mock HA");
});
