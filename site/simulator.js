const DEFAULT_PROMPT = `You are ESPClaw Browser Lab, using the real ESPClaw C runtime compiled to WebAssembly.

Choose the execution shape semantically:
- one-shot action now => direct tool calls
- repeated or timed behavior requested now => execute the full sequence, not a single write
- reusable logic => app.install or component.install
- run reusable logic now => task.start after install
- persist or autostart reusable logic => behavior.register after install

Available runtime primitives:
- component: reusable module or driver
- app: installable feature
- task: live running instance
- behavior: persisted task definition
- event: decoupled local signal

Rules:
- respond as strict JSON only
- schema:
  {"assistant":"string","tool_calls":[{"name":"tool.name","arguments":{}}],"done":true|false}
- if you need tools, set done=false
- if you can answer finally, set done=true
- never invent runtime types outside components, apps, tasks, behaviors, and events
- use system.logs when asked to inspect recent runtime activity
`;

const FALLBACK_MODELS = [
  "Llama-3.2-1B-Instruct-q4f16_1-MLC",
  "Qwen2.5-1.5B-Instruct-q4f16_1-MLC",
  "Phi-3.5-mini-instruct-q4f16_1-MLC",
];

const WASM_STATUS_PENDING = {
  board_profile: "esp32cam",
  storage_backend: "memfs",
  workspace_ready: false,
  workspace_root: "/workspace",
  channel: "browser_lab",
  components: 0,
  apps: 0,
  tasks: 0,
  behaviors: 0,
};

const TOOL_DESCRIPTIONS = [
  { name: "system.info", description: "Inspect browser-simulated board, storage, and runtime health." },
  { name: "system.logs", description: "Read the in-memory runtime log tail." },
  { name: "hardware.list", description: "Inspect board descriptor, named pins and capabilities." },
  { name: "app_patterns.list", description: "Read architecture guidance for components, apps, tasks, behaviors, and events." },
  { name: "tool.list", description: "List available tool names and summaries." },
  { name: "fs.list", description: "List files in the simulator workspace." },
  { name: "fs.read", description: "Read a workspace file." },
  { name: "fs.write", description: "Write a workspace file." },
  { name: "component.list", description: "List installed reusable components." },
  { name: "component.install", description: "Install a reusable component from inline source." },
  { name: "component.install_from_url", description: "Install a reusable component from a remote source URL." },
  { name: "component.install_from_manifest", description: "Install a component from a manifest URL." },
  { name: "app.list", description: "List installed apps." },
  { name: "app.install", description: "Install a Lua app." },
  { name: "task.list", description: "List running tasks." },
  { name: "task.start", description: "Start a task." },
  { name: "behavior.list", description: "List persisted behaviors." },
  { name: "behavior.register", description: "Persist a behavior." },
  { name: "event.emit", description: "Emit a named event payload." },
  { name: "camera.capture", description: "Capture a simulated JPEG frame and persist it into media/." },
];

function safeJsonParse(value) {
  if (!value) return null;
  try {
    return JSON.parse(value);
  } catch (_error) {
    const match = value.match(/\{[\s\S]*\}/);
    if (!match) return null;
    try {
      return JSON.parse(match[0]);
    } catch (_inner) {
      return null;
    }
  }
}

function truncate(text, max = 140) {
  if (!text) return "";
  if (text.length <= max) return text;
  return `${text.slice(0, max - 1)}…`;
}

function htmlEscape(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

class WasmKernel {
  constructor() {
    this.module = null;
    this.loading = null;
    this.statusCache = { ...WASM_STATUS_PENDING };
    this.toolCatalog = [...TOOL_DESCRIPTIONS];
  }

  snapshot() {
    const asCount = (value) => {
      const numeric = Number(value);
      return Number.isFinite(numeric) ? numeric : 0;
    };
    return {
      board: this.statusCache.board_profile || "esp32cam",
      workspaceFiles: asCount(this.statusCache.workspace_files),
      components: asCount(this.statusCache.components),
      apps: asCount(this.statusCache.apps),
      tasks: asCount(this.statusCache.tasks),
      behaviors: asCount(this.statusCache.behaviors),
    };
  }

  async ensureReady() {
    if (this.module) {
      return this.module;
    }
    if (this.loading) {
      return this.loading;
    }
    this.loading = this.bootstrap();
    try {
      this.module = await this.loading;
      return this.module;
    } finally {
      this.loading = null;
    }
  }

  async bootstrap() {
    const imported = await import("./wasm/espclaw-browser-runtime.js");
    const factory = imported.default || imported.ESPClawBrowserModule;
    if (typeof factory !== "function") {
      throw new Error("WASM module factory was not exported.");
    }
    const module = await factory();
    this.readCStringFrom(module, module._espclaw_wasm_reset());
    this.module = module;
    this.refreshStatus();
    this.refreshToolCatalog();
    return module;
  }

  readCStringFrom(module, pointer) {
    return module?.UTF8ToString(pointer) || "";
  }

  readCString(pointer) {
    return this.readCStringFrom(this.module, pointer);
  }

  allocCString(value) {
    const text = String(value ?? "");
    const bytes = this.module.lengthBytesUTF8(text) + 1;
    const pointer = this.module._malloc(bytes);
    this.module.stringToUTF8(text, pointer, bytes);
    return pointer;
  }

  parseJson(text, fallbackMessage) {
    const parsed = safeJsonParse(text);
    if (parsed) {
      return parsed;
    }
    return { ok: false, error: fallbackMessage, raw: text };
  }

  refreshStatus() {
    const payload = this.parseJson(this.readCString(this.module._espclaw_wasm_status_json()), "status_parse_failed");
    if (payload && typeof payload === "object") {
      this.statusCache = {
        ...this.statusCache,
        ...payload,
      };
    }
    const info = this.execute("system.info", {}, false);
    if (info && typeof info === "object") {
      this.statusCache = {
        ...this.statusCache,
        ...info,
      };
    }
    return this.statusCache;
  }

  refreshToolCatalog() {
    const result = this.execute("tool.list", {});
    if (result?.ok && Array.isArray(result.tools) && result.tools.length) {
      this.toolCatalog = result.tools.map((tool) => ({
        name: tool.name,
        description: tool.summary || tool.description || "",
      }));
    }
    return this.toolCatalog;
  }

  logs(bytes = 4096) {
    const payload = this.parseJson(this.readCString(this.module._espclaw_wasm_logs_json(bytes)), "logs_parse_failed");
    return payload;
  }

  execute(name, args = {}, allowMutations = true) {
    if (!this.module) {
      throw new Error("WASM runtime is not initialized yet.");
    }
    const namePtr = this.allocCString(name);
    const argsPtr = this.allocCString(JSON.stringify(args || {}));
    try {
      const resultPtr = this.module._espclaw_wasm_execute_tool(namePtr, argsPtr, allowMutations ? 1 : 0);
      const payload = this.parseJson(this.readCString(resultPtr), "tool_result_parse_failed");
      if (name === "system.info") {
        this.statusCache = {
          ...this.statusCache,
          ...payload,
        };
      }
      return payload;
    } finally {
      this.module._free(namePtr);
      this.module._free(argsPtr);
    }
  }

  async reset() {
    await this.ensureReady();
    const payload = this.parseJson(this.readCString(this.module._espclaw_wasm_reset()), "reset_parse_failed");
    this.refreshStatus();
    this.refreshToolCatalog();
    return payload;
  }
}

function buildSystemPrompt(kernel) {
  const toolText = (kernel.toolCatalog || TOOL_DESCRIPTIONS)
    .map((tool) => `- ${tool.name}: ${tool.description}`)
    .join("\n");
  const snapshot = kernel.snapshot();
  return `${DEFAULT_PROMPT}

Current runtime snapshot:
- board: ${snapshot.board}
- workspace files: ${snapshot.workspaceFiles}
- components: ${snapshot.components}
- apps: ${snapshot.apps}
- tasks: ${snapshot.tasks}
- behaviors: ${snapshot.behaviors}

Available tools:
${toolText}
`;
}

async function loadWebLlmModule() {
  return import("https://esm.run/@mlc-ai/web-llm");
}

function chooseDefaultModel(models) {
  const candidates = models.filter(Boolean);
  return (
    candidates.find((name) => /qwen|phi|llama-3\.2-1b/i.test(name)) ||
    candidates.find((name) => /1b|1\.5b|3b/i.test(name)) ||
    candidates[0] ||
    FALLBACK_MODELS[0]
  );
}

export class BrowserLab {
  constructor(root) {
    this.root = root;
    this.kernel = new WasmKernel();
    this.provider = "webllm";
    this.webllm = null;
    this.webllmEngine = null;
    this.messages = [];
    this.trace = [];
    this.modelOptions = [...FALLBACK_MODELS];
    this.elements = this.captureElements();
    this.bind();
    window.__espclawBrowserLabSeed = (seed) => this.seedInput(seed);
    this.renderRuntime();
    this.renderTrace();
    void this.renderLogs();
    this.seedWelcome();
    void this.initializeRuntime();
    void this.bootstrapModels();
  }

  captureElements() {
    return {
      providerButtons: [...this.root.querySelectorAll("[data-provider]")],
      modelSelect: this.root.querySelector("#lab-model-select"),
      initButton: this.root.querySelector("#lab-init-button"),
      resetButton: this.root.querySelector("#lab-reset-button"),
      sendButton: this.root.querySelector("#lab-send-button"),
      input: this.root.querySelector("#lab-input"),
      composer: this.root.querySelector("#lab-composer"),
      transcript: this.root.querySelector("#lab-transcript"),
      trace: this.root.querySelector("#lab-trace"),
      clearTrace: this.root.querySelector("#lab-clear-trace"),
      refreshLogs: this.root.querySelector("#lab-refresh-logs"),
      logConsole: this.root.querySelector("#lab-log-console"),
      runtimeStats: this.root.querySelector("#lab-runtime-stats"),
      engineStatus: this.root.querySelector("#lab-engine-status"),
      engineProgress: this.root.querySelector("#lab-engine-progress"),
      turnBadge: this.root.querySelector("#lab-turn-badge"),
      baseUrl: this.root.querySelector("#lab-openai-base-url"),
      apiKey: this.root.querySelector("#lab-openai-api-key"),
      providerOpenAiFields: [...this.root.querySelectorAll(".field-provider-openai")],
      scenarioButtons: [...this.root.querySelectorAll(".scenario-chip")],
    };
  }

  bind() {
    for (const button of this.elements.providerButtons) {
      button.addEventListener("click", () => this.setProvider(button.dataset.provider || "webllm"));
    }
    this.root.addEventListener("click", (event) => {
      const button = event.target.closest(".scenario-chip");
      if (!button) return;
      this.seedInput(button.dataset.seed || "");
    });
    this.elements.initButton.addEventListener("click", () => void this.initializeProvider());
    this.elements.resetButton.addEventListener("click", () => void this.resetLab());
    this.elements.clearTrace.addEventListener("click", () => {
      this.trace = [];
      this.renderTrace();
    });
    this.elements.refreshLogs.addEventListener("click", () => void this.renderLogs());
    this.elements.composer.addEventListener("submit", (event) => {
      event.preventDefault();
      void this.handleSubmit();
    });
  }

  seedInput(seed) {
    this.elements.input.value = seed || "";
    this.elements.input.focus();
  }

  seedWelcome() {
    this.pushMessage(
      "system",
      "Browser Lab is loading the real ESPClaw runtime compiled to WebAssembly. Initialize a model, then ask it to install components, create apps, run tasks, inspect system.logs, or explain the ESPClaw architecture.",
    );
  }

  async initializeRuntime() {
    try {
      this.updateEngineStatus("Booting ESPClaw WASM runtime…", 6);
      await this.kernel.ensureReady();
      this.renderRuntime();
      await this.renderLogs();
      this.updateEngineStatus("ESPClaw WASM runtime ready. Initialize a model to chat locally.", 0);
    } catch (error) {
      this.updateEngineStatus(`WASM runtime failed to boot: ${error.message}`, 0);
      this.pushMessage("system", `WASM runtime failed to initialize: ${error.message}`);
    }
  }

  async bootstrapModels() {
    try {
      this.webllm = await loadWebLlmModule();
      const modelList = this.webllm?.prebuiltAppConfig?.model_list?.map((item) => item.model_id).filter(Boolean);
      if (modelList?.length) {
        this.modelOptions = modelList;
      }
      this.renderModelOptions();
      this.updateEngineStatus("WebLLM module loaded. Initialize a model to chat locally.", 0);
    } catch (_error) {
      this.renderModelOptions();
      this.updateEngineStatus("WebLLM module could not be loaded automatically. You can still use an OpenAI-compatible endpoint.", 0);
    }
  }

  renderModelOptions() {
    const selected = this.elements.modelSelect.value || chooseDefaultModel(this.modelOptions);
    this.elements.modelSelect.innerHTML = this.modelOptions
      .map((model) => `<option value="${htmlEscape(model)}"${model === selected ? " selected" : ""}>${htmlEscape(model)}</option>`)
      .join("");
  }

  setProvider(provider) {
    this.provider = provider;
    for (const button of this.elements.providerButtons) {
      button.classList.toggle("is-active", button.dataset.provider === provider);
    }
    for (const field of this.elements.providerOpenAiFields) {
      field.classList.toggle("hidden", provider !== "openai");
    }
    this.elements.turnBadge.textContent = provider === "webllm" ? "Browser-local" : "Endpoint-backed";
  }

  updateEngineStatus(message, progress = null) {
    this.elements.engineStatus.textContent = message;
    if (typeof progress === "number") {
      const clamped = Math.max(0, Math.min(100, progress));
      this.elements.engineProgress.style.width = `${clamped}%`;
    }
  }

  async initializeProvider() {
    if (this.provider === "openai") {
      this.updateEngineStatus("OpenAI-compatible provider configured. The browser lab will use your endpoint on the next turn.", 100);
      return;
    }

    try {
      if (!this.webllm) {
        this.webllm = await loadWebLlmModule();
      }
      const model = this.elements.modelSelect.value || chooseDefaultModel(this.modelOptions);
      this.updateEngineStatus(`Loading ${model}…`, 4);
      this.webllmEngine = await this.webllm.CreateMLCEngine(model, {
        initProgressCallback: (report) => {
          const progress = typeof report.progress === "number" ? report.progress * 100 : 0;
          this.updateEngineStatus(report.text || `Loading ${model}…`, progress);
        },
      });
      this.updateEngineStatus(`WebLLM ready: ${model}`, 100);
    } catch (error) {
      this.updateEngineStatus(`Model initialization failed: ${error.message}`, 0);
    }
  }

  async resetLab() {
    await this.kernel.reset();
    this.messages = [];
    this.trace = [];
    this.renderTrace();
    await this.renderLogs();
    this.renderRuntime();
    this.elements.input.value = "";
    this.seedWelcome();
  }

  pushMessage(role, text) {
    this.messages.push({ role, text });
    const article = document.createElement("article");
    article.className = `message message-${role}`;
    article.innerHTML = `<strong>${htmlEscape(role)}</strong><p>${htmlEscape(text)}</p>`;
    this.elements.transcript.appendChild(article);
    this.elements.transcript.scrollTop = this.elements.transcript.scrollHeight;
  }

  renderTrace() {
    if (!this.trace.length) {
      this.elements.trace.innerHTML = `<p class="trace-empty">No tool calls yet.</p>`;
      return;
    }
    this.elements.trace.innerHTML = this.trace
      .slice()
      .reverse()
      .map(
        (entry) => `
          <article class="trace-entry">
            <strong>${htmlEscape(entry.name)}</strong>
            <p>${htmlEscape(entry.summary)}</p>
          </article>
        `,
      )
      .join("");
  }

  async renderLogs() {
    try {
      await this.kernel.ensureReady();
    } catch (_error) {
      this.elements.logConsole.textContent = "WASM runtime is still loading.";
      return;
    }
    const result = this.kernel.logs(2200);
    const tail = result.tail || "";
    this.elements.logConsole.textContent = tail || "No logs captured yet.";
  }

  renderRuntime() {
    const snapshot = this.kernel.snapshot();
    const stats = [
      ["Board profile", snapshot.board],
      ["Workspace files", String(snapshot.workspaceFiles)],
      ["Components", String(snapshot.components)],
      ["Apps", String(snapshot.apps)],
      ["Tasks", String(snapshot.tasks)],
      ["Behaviors", String(snapshot.behaviors)],
    ];
    this.elements.runtimeStats.innerHTML = stats
      .map(
        ([label, value]) => `
          <article class="mini-stat">
            <span>${htmlEscape(label)}</span>
            <strong>${htmlEscape(value)}</strong>
          </article>
        `,
      )
      .join("");
  }

  async handleSubmit() {
    const text = this.elements.input.value.trim();
    if (!text) return;
    this.elements.input.value = "";
    this.pushMessage("user", text);
    await this.runAgentLoop(text);
  }

  async runAgentLoop(text) {
    await this.kernel.ensureReady();
    const conversation = [{ role: "user", content: text }];
    for (let iteration = 0; iteration < 6; iteration += 1) {
      const plan = await this.requestPlan(conversation);
      if (!plan) {
        this.pushMessage("assistant", "I couldn’t parse a planner response from the selected model.");
        return;
      }

      if (Array.isArray(plan.tool_calls) && plan.tool_calls.length) {
        const toolResults = [];
        for (const call of plan.tool_calls) {
          const result = this.kernel.execute(call.name, call.arguments || {});
          this.trace.push({
            name: call.name,
            summary: JSON.stringify(result, null, 2),
          });
          toolResults.push({ name: call.name, result });
        }
        this.renderTrace();
        await this.renderLogs();
        this.renderRuntime();
        conversation.push({
          role: "assistant",
          content: JSON.stringify({
            assistant: plan.assistant || "",
            tool_calls: plan.tool_calls,
            done: false,
          }),
        });
        conversation.push({
          role: "user",
          content: `Tool results:\n${JSON.stringify(toolResults, null, 2)}\nContinue until the request is actually complete.`,
        });
        continue;
      }

      this.pushMessage("assistant", plan.assistant || "Done.");
      return;
    }

    this.pushMessage("assistant", "I hit the browser-lab iteration limit before reaching a final answer.");
  }

  async requestPlan(conversation) {
    const messages = [
      { role: "system", content: buildSystemPrompt(this.kernel) },
      ...conversation,
      {
        role: "user",
        content:
          "Respond now as strict JSON only with assistant, tool_calls, and done. If tools are needed, emit them instead of narrating.",
      },
    ];
    const raw = await this.complete(messages);
    return safeJsonParse(raw);
  }

  async complete(messages) {
    if (this.provider === "openai") {
      return this.completeOpenAi(messages);
    }
    return this.completeWebLlm(messages);
  }

  async completeOpenAi(messages) {
    const baseUrl = (this.elements.baseUrl.value || "https://api.openai.com/v1").replace(/\/$/, "");
    const apiKey = this.elements.apiKey.value.trim();
    const response = await fetch(`${baseUrl}/chat/completions`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        ...(apiKey ? { Authorization: `Bearer ${apiKey}` } : {}),
      },
      body: JSON.stringify({
        model: this.elements.modelSelect.value || "gpt-4.1-mini",
        temperature: 0.2,
        response_format: { type: "json_object" },
        messages,
      }),
    });
    if (!response.ok) {
      throw new Error(`OpenAI-compatible endpoint returned ${response.status}`);
    }
    const payload = await response.json();
    return payload.choices?.[0]?.message?.content || "";
  }

  async completeWebLlm(messages) {
    if (!this.webllmEngine) {
      throw new Error("Initialize the WebLLM model first.");
    }
    const response = await this.webllmEngine.chat.completions.create({
      messages,
      temperature: 0.2,
      max_tokens: 700,
      response_format: { type: "json_object" },
    });
    return response.choices?.[0]?.message?.content || "";
  }
}
