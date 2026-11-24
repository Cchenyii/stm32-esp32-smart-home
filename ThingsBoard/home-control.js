self.onInit = function () {
    const root = self.ctx.$container[0].querySelector(".edge-cloud-home");
    const api = self.ctx.controlApi;
    if (!root || !api) return;
    const controls = [...root.querySelectorAll("button,input")];
    const fields = {};
    for (const el of root.querySelectorAll("[data-field]")) fields[el.dataset.field] = el;

    const resultText = [
        "STM32 已确认", "参数无效", "操作不受支持", "另有一条命令正在处理",
        "请先切换手动模式", "本地操作优先，请稍后重试", "状态已变化，请刷新后操作",
        "STM32 已接受，舵机等待动作间隔", "STM32 暂不可用", "STM32 应答超时，结果未知"
    ];
    const positions = ["待命", "开", "关"];
    const maxId = 0xffffffff;
    let alive = true;
    let active = null;
    let snapshot = null;
    let snapshotAt = 0;
    let uncertain = false;
    let lastSentSession = 0;
    let lastSentId = 0;

    function set(field, value) {
        fields[field].textContent = value;
    }

    function numberIn(value, low, high) {
        return Number.isInteger(value) && value >= low && value <= high;
    }

    function ready() {
        return snapshot !== null && Date.now() - snapshotAt < 12000;
    }

    function render() {
        const data = ready() ? snapshot : null;
        set("connection", data ? "STM32 在线" : "设备离线或等待数据");
        set("temperature", data?.temperature_valid && numberIn(data.temperature_tenths, -400, 800)
            ? (data.temperature_tenths / 10).toFixed(1) + " °C" : "—");
        set("humidity", data?.temperature_valid && numberIn(data.humidity_tenths, 0, 1000)
            ? (data.humidity_tenths / 10).toFixed(1) + " %" : "—");
        set("light", data?.light_valid && numberIn(data.light_level, 0, 4095)
            ? data.light_level + " / 4095" : "—");
        set("mode", data ? (data.manual ? "手动" : "自动") : "—");
        set("fan", data ? (data.fan_on ? "开" : "关") : "—");
        set("curtain", data ? positions[data.curtain] +
            (data.curtain !== data.curtain_target ? " → " + positions[data.curtain_target] : "") : "—");
        set("heatThreshold", data ? "开 " + data.fan_on_tenths / 10 +
            " °C / 停 " + (data.fan_on_tenths - 20) / 10 + " °C" : "—");
        set("lightThreshold", data ? "关 " + data.close_level +
            " / 开 " + (data.close_level - 500) : "—");
        for (const el of controls) {
            el.disabled = !data || (active !== null && (el.tagName !== "INPUT" || active.method === "setHome")) ||
                uncertain ||
                (el.hasAttribute("data-manual") && !data.manual);
        }
    }

    function rpc(method, params, completion) {
        const request = { method, subscription: null };
        active = request;
        render();
        function finish(ok, value) {
            if (!alive || active !== request) return;
            active = null;
            api.completedCommand();
            completion(ok, value);
            render();
        }
        try {
            request.subscription = api.sendTwoWayCommand(method, params, 6000).subscribe(
                value => finish(true, value), () => finish(false, null));
        } catch (_) {
            finish(false, null);
        }
    }

    function readState() {
        if (!alive || active !== null || document.hidden) return;
        rpc("getHome", {}, (ok, data) => {
            if (!ok || data?.uart_online !== true || !numberIn(data.session, 1, maxId) ||
                !numberIn(data.last_id, 0, maxId) || typeof data.manual !== "boolean" ||
                typeof data.fan_on !== "boolean" || !numberIn(data.curtain, 0, 2) ||
                !numberIn(data.curtain_target, 0, 2) ||
                !numberIn(data.fan_on_tenths, 100, 600) || !numberIn(data.close_level, 600, 4000)) {
                snapshot = null;
                return;
            }
            snapshot = data;
            snapshotAt = Date.now();
            uncertain = false;
        });
    }

    function send(op, value) {
        if (!alive || active !== null || uncertain || !ready()) return;
        const lastId = Math.max(snapshot.last_id, lastSentSession === snapshot.session ? lastSentId : 0);
        if (lastId === maxId) {
            set("notice", "会话编号已用完，请重连网关");
            return;
        }
        const command = { session: snapshot.session, id: lastId + 1, op, value };
        lastSentSession = command.session;
        lastSentId = command.id; // A timeout must not reuse this ID for a different target.
        set("notice", "等待 STM32 应答…");
        rpc("setHome", command, (ok, reply) => {
            const matches = ok && reply && reply.session === command.session &&
                reply.id === command.id && reply.op === op && numberIn(reply.result, 0, 9) &&
                typeof reply.confirmed === "boolean";
            if (matches && (reply.confirmed || ![0, 7].includes(reply.result))) {
                set("notice", resultText[reply.result]);
            } else {
                uncertain = true;
                set("notice", "这条命令未获确认，请核对刷新后的设备状态");
            }
            readState();
        });
    }

    const clicked = event => {
        const button = event.target.closest("button[data-op]");
        if (button && !button.disabled) send(Number(button.dataset.op), Number(button.dataset.value));
    };
    const submitted = event => {
        const form = event.target;
        if (!form.matches("form[data-op]")) return;
        event.preventDefault();
        if (!form.reportValidity()) return;
        const op = Number(form.dataset.op);
        const value = Number(form.querySelector("input").value);
        send(op, op === 4 ? value * 10 : value);
    };
    root.addEventListener("click", clicked);
    root.addEventListener("submit", submitted);
    document.addEventListener("visibilitychange", readState);
    const timer = setInterval(readState, 7000);
    self.cleanupHomeWidget = function () {
        alive = false;
        clearInterval(timer);
        root.removeEventListener("click", clicked);
        root.removeEventListener("submit", submitted);
        document.removeEventListener("visibilitychange", readState);
        if (active) {
            active.subscription?.unsubscribe();
            api.completedCommand();
            active = null;
        }
    };
    readState();
};

self.onDestroy = function () {
    self.cleanupHomeWidget?.();
};
