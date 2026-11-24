const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const controls = [
    { dataset: {op: "1", value: "1"}, tagName: "BUTTON", hasAttribute: () => false },
    { dataset: {op: "2", value: "1"}, tagName: "BUTTON", hasAttribute: name => name === "data-manual" }
];
const fieldNames = ["connection", "temperature", "humidity", "light", "mode", "fan", "curtain",
    "heatThreshold", "lightThreshold", "notice"];
const fields = Object.fromEntries(fieldNames.map(field => [field, {dataset: {field}}]));
const handlers = {};
const root = {
    querySelectorAll: selector => selector === "button,input" ? controls : Object.values(fields),
    addEventListener: (type, callback) => { handlers[type] = callback; },
    removeEventListener: type => { delete handlers[type]; }
};
const requests = [];
let interval;
let cleaned = false;
const self = {ctx: {
    $container: [{querySelector: () => root}],
    controlApi: {
        completedCommand() {},
        sendTwoWayCommand(method, params) {
            return {subscribe(next, error) {
                const request = {method, params, next, error, cancelled: false};
                requests.push(request);
                return {unsubscribe: () => { request.cancelled = true; }};
            }};
        }
    }
}};
const document = {hidden: false, addEventListener() {}, removeEventListener() {}};
const source = fs.readFileSync(path.join(__dirname, "../ThingsBoard/home-control.js"), "utf8");
vm.runInNewContext(source, {
    self, document, setInterval: callback => { interval = callback; return 1; },
    clearInterval: () => { cleaned = true; }
});
const state = {
    uart_online: true, session: 123, last_id: 0, manual: false, fan_on: false,
    curtain: 1, curtain_target: 1, fan_on_tenths: 300, close_level: 3000,
    temperature_valid: true, temperature_tenths: 246, humidity_tenths: 583,
    light_valid: true, light_level: 2760
};

function take(method) {
    const request = requests.shift();
    assert.ok(request);
    assert.equal(request.method, method);
    return request;
}

function click(control) {
    handlers.click({target: {closest: () => control}});
}

self.onInit();
assert.ok(controls[1].disabled);
take("getHome").next(state);
assert.equal(fields.temperature.textContent, "24.6 °C");
assert.ok(controls[1].disabled);
click(controls[1]);
assert.equal(requests.length, 0);

click(controls[0]);
let command = take("setHome");
assert.equal(command.params.session, 123);
assert.equal(command.params.id, 1);
command.next({...command.params, result: 0, confirmed: true});
take("getHome").next({...state, last_id: 1, manual: true});
assert.ok(!controls[1].disabled);

click(controls[1]);
command = take("setHome");
assert.equal(command.params.id, 2);
command.next({...command.params, session: 456, result: 0, confirmed: true});
assert.match(fields.notice.textContent, /未获确认/);
take("getHome").next({...state, last_id: 1, manual: true});
click(controls[1]);
command = take("setHome");
assert.equal(command.params.id, 3);
command.error();
take("getHome").next({uart_online: false});
assert.ok(controls[1].disabled);
assert.equal(fields.temperature.textContent, "—");

interval();
take("getHome").next({...state, session: 456, manual: true});
click(controls[1]);
command = take("setHome");
assert.equal(command.params.session, 456);
assert.equal(command.params.id, 1);
self.onDestroy();
assert.ok(command.cancelled && cleaned);
assert.equal(requests.length, 0);
console.log("ThingsBoard widget: manual guard, correlated ACK, no ID reuse, offline and reconnect passed");

const widgetDir = path.join(__dirname, "..", "ThingsBoard");
const readAsset = file => fs.readFileSync(path.join(widgetDir, file), "utf8");
const widget = JSON.parse(readAsset("home-control-widget.json"));
const dashboard = JSON.parse(readAsset("home-control-dashboard.json")).configuration;
const normalize = content => content.replace(/\r\n/g, "\n").trimEnd();
for (const [field, extension] of [
    ["templateHtml", "html"], ["templateCss", "css"], ["controllerScript", "js"]
]) {
    assert.equal(normalize(widget.descriptor[field]), normalize(readAsset("home-control." + extension)));
}
for (const entry of Object.values(dashboard.widgets)) {
    assert.equal(entry.typeFullFqn, "tenant." + widget.fqn);
    assert.equal(dashboard.entityAliases[entry.config.targetDevice.entityAliasId].filter.type, "entityName");
}
console.log("ThingsBoard imports: widget assets and dashboard alias passed");
